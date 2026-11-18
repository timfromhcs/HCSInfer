// SessionManager tests. Two layers:
//   * a scripted FakeLauncher (deterministic: policy kills, caps, timeouts, cancel, shutdown, races);
//   * the real hcs::process launcher driving hcs_child_probe as a stand-in for prime-agent (Job Object,
//   separate
//     pipes, replaced environment, whole-tree kill). A real prime-agent session is NOT RUN (docs/agent.md).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "hcs/agent/session_manager.hpp"
#include "hcs/logger.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/process/launcher.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::agent;
using hcs::test::TempDir;
using hcs::test::write_file;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kToken = "tok-SECRET-0123456789abcdef";

// ------------------------------------------------------------------------------------------------ fake
// launcher
class ByteQueue {
public:
    void push(const std::string& s) {
        std::lock_guard lk(m_);
        data_ += s;
        cv_.notify_all();
    }
    void close() {
        std::lock_guard lk(m_);
        closed_ = true;
        cv_.notify_all();
    }
    std::size_t read(char* buf, std::size_t cap) {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return !data_.empty() || closed_; });
        const std::size_t n = std::min(cap, data_.size());
        std::copy_n(data_.data(), n, buf);
        data_.erase(0, n);
        return n;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::string data_;
    bool closed_ = false;
};

struct FakeChild {
    ByteQueue out;
    ByteQueue err;
    std::atomic<int> terminate_calls{0};
    std::mutex m;
    std::condition_variable cv;
    std::optional<int> code;

    void finish(int c) {
        {
            std::lock_guard lk(m);
            if (!code)
                code = c;
        }
        out.close();
        err.close();
        cv.notify_all();
    }
    void line(const std::string& l) { out.push(l + "\n"); }
};

class ChildHandle final : public process::IChildProcess {
public:
    explicit ChildHandle(std::shared_ptr<FakeChild> c) : c_(std::move(c)) {}
    std::uint32_t pid() const override { return 4242; }
    std::size_t read_stdout(char* b, std::size_t n) override { return c_->out.read(b, n); }
    std::size_t read_stderr(char* b, std::size_t n) override { return c_->err.read(b, n); }
    void terminate() override {
        ++c_->terminate_calls;
        c_->finish(1);
    }
    std::optional<int> wait(std::uint32_t ms) override {
        std::unique_lock lk(c_->m);
        c_->cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return c_->code.has_value(); });
        return c_->code;
    }

private:
    std::shared_ptr<FakeChild> c_;
};

struct LauncherState {
    std::mutex m;
    std::vector<std::shared_ptr<FakeChild>> children;
    std::vector<process::LaunchSpec> specs;
    bool fail = false;
    std::function<void(FakeChild&)> on_launch;

    std::shared_ptr<FakeChild> child(std::size_t i, int timeout_ms = 5000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lk(m);
                if (children.size() > i)
                    return children[i];
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return nullptr;
    }
    process::LaunchSpec spec(std::size_t i) {
        std::lock_guard lk(m);
        return specs.at(i);
    }
};

class FakeLauncher final : public process::IProcessLauncher {
public:
    explicit FakeLauncher(std::shared_ptr<LauncherState> s) : s_(std::move(s)) {}
    Result<std::unique_ptr<process::IChildProcess>> launch(const process::LaunchSpec& spec) override {
        auto c = std::make_shared<FakeChild>();
        std::function<void(FakeChild&)> hook;
        {
            std::lock_guard lk(s_->m);
            if (s_->fail)
                return make_error(ErrorCode::IoError, "scripted launch failure");
            s_->children.push_back(c);
            s_->specs.push_back(spec);
            hook = s_->on_launch;
        }
        if (hook)
            hook(*c);
        return std::unique_ptr<process::IChildProcess>(new ChildHandle(c));
    }

private:
    std::shared_ptr<LauncherState> s_;
};

// ------------------------------------------------------------------------------------------------ event
// lines
std::string turn(const std::string& text) {
    return json{{"type", "turn_end"},
                {"message",
                 {{"role", "assistant"},
                  {"content", text},
                  {"stopReason", "stop"},
                  {"usage", {{"input", 10}, {"output", 5}}}}}}
        .dump();
}
std::string tool_call(const std::string& code, const std::string& tool = "python_repl") {
    return json{{"type", "tool_execution_start"},
                {"toolName", tool},
                {"toolCallId", "c1"},
                {"args", {{"code", code}}}}
        .dump();
}
std::string tool_end(const std::string& text) {
    return json{{"type", "tool_execution_end"},
                {"toolName", "python_repl"},
                {"toolCallId", "c1"},
                {"isError", false},
                {"result", {{"content", json::array({{{"type", "text"}, {"text", text}}})}}}}
        .dump();
}
const std::string kStart = R"({"type":"agent_start"})";
const std::string kEnd = R"({"type":"agent_end"})";

// ------------------------------------------------------------------------------------------------ fixture
struct Fixture {
    TempDir tmp;
    fs::path ws = tmp / "ws";
    fs::path state = tmp / "state";
    fs::path exe = tmp / "bin" / "agent.exe";
    fs::path py = tmp / "bin" / "python.exe";
    std::ostringstream log_stream;
    Logger log{&log_stream, LogLevel::Debug};
    std::shared_ptr<LauncherState> ls = std::make_shared<LauncherState>();
    AgentSettings settings;
    std::unique_ptr<SessionManager> mgr;

    Fixture() {
        fs::create_directories(ws);
        write_file(exe, "x");
        write_file(py, "x");
        settings.agent_exe = exe;
        settings.kernel_python = py;
        settings.state_dir = state;
        settings.base_url = "http://127.0.0.1:8765/v1";
        settings.token = kToken;
        settings.model_id = "lfm3-omni-2b-q8_0";
        settings.limits.kill_grace_ms = 300;
        settings.limits.default_timeout_ms = 20000;
    }

    PathGuard roots() const {
        PathGuard g;
        EXPECT_TRUE(g.add_root(ws));
        return g;
    }

    void start() {
        mgr = std::make_unique<SessionManager>(settings, roots(), std::make_unique<FakeLauncher>(ls), log);
    }
    void start_with(std::unique_ptr<process::IProcessLauncher> l) {
        mgr = std::make_unique<SessionManager>(settings, roots(), std::move(l), log);
    }

    CreateRequest req(const std::string& task = "fix the bug") const {
        CreateRequest r;
        r.root = ws;
        r.task = task;
        return r;
    }

    std::string create_ok(const std::string& task = "fix the bug",
                          std::optional<std::uint32_t> timeout = {}) {
        CreateRequest r = req(task);
        r.timeout_ms = timeout;
        auto id = mgr->create(r);
        EXPECT_TRUE(id) << (id ? "" : id.error().message);
        return id ? id.value() : std::string();
    }

    SessionInfo wait_terminal(const std::string& id, int timeout_ms = 10000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        SessionInfo info;
        while (std::chrono::steady_clock::now() < deadline) {
            auto g = mgr->get(id);
            if (g) {
                info = g.value();
                if (is_terminal(info.state))
                    return info;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ADD_FAILURE() << "session did not finish within " << timeout_ms << " ms, state "
                      << to_string(info.state);
        return info;
    }

    std::vector<AgentEvent> all_events(const std::string& id) {
        std::vector<AgentEvent> out;
        std::uint64_t next = 0;
        for (;;) {
            auto page = mgr->events(id, next, 0, 1000);
            if (!page || page.value().events.empty())
                break;
            for (auto& e : page.value().events) out.push_back(e);
            next = page.value().next_seq;
        }
        return out;
    }
};

std::vector<std::string> kinds(const std::vector<AgentEvent>& evs) {
    std::vector<std::string> k;
    for (const auto& e : evs) k.push_back(e.kind);
    return k;
}

std::size_t count_kind(const std::vector<AgentEvent>& evs, const std::string& kind) {
    return static_cast<std::size_t>(
        std::count_if(evs.begin(), evs.end(), [&](const auto& e) { return e.kind == kind; }));
}

bool any_contains(const std::vector<AgentEvent>& evs, const std::string& needle) {
    for (const auto& e : evs)
        if (to_json(e).dump().find(needle) != std::string::npos)
            return true;
    return false;
}

std::string slurp(const fs::path& p) {
    return hcs::test::read_file(p);
}

}  // namespace

// ================================================================================================================
// Creation: every bad input fails closed, synchronously, without launching anything
// ================================================================================================================
TEST(SessionCreate, RejectsBadTasks) {
    Fixture f;
    f.start();
    auto bad = [&](const std::string& task, std::optional<std::uint32_t> timeout, ErrorCode code,
                   const char* why) {
        CreateRequest r = f.req(task);
        r.timeout_ms = timeout;
        auto id = f.mgr->create(r);
        ASSERT_FALSE(id) << why;
        EXPECT_EQ(id.error().code, code) << why;
    };
    bad("", std::nullopt, ErrorCode::InvalidArgument, "empty");
    bad(std::string(f.settings.limits.max_task_bytes + 1, 'a'), std::nullopt, ErrorCode::InvalidArgument,
        "too long");
    bad(std::string("a\0b", 3), std::nullopt, ErrorCode::InvalidArgument, "NUL");
    bad("ok", 0u, ErrorCode::InvalidArgument, "zero timeout");
    EXPECT_EQ(f.ls->children.size(), 0u);
}

TEST(SessionCreate, RejectsBadRoots) {
    Fixture f;
    f.start();
    auto expect_fail = [&](const fs::path& root, std::initializer_list<ErrorCode> codes, const char* why) {
        CreateRequest r = f.req();
        r.root = root;
        auto id = f.mgr->create(r);
        ASSERT_FALSE(id) << why;
        EXPECT_NE(std::find(codes.begin(), codes.end(), id.error().code), codes.end())
            << why << ": " << id.error().message;
    };
    expect_fail("relative/dir", {ErrorCode::InvalidArgument}, "relative root");
    expect_fail(f.tmp / "elsewhere",
                {ErrorCode::SecurityViolation, ErrorCode::InvalidArgument, ErrorCode::NotFound}, "outside");
    expect_fail(f.ws / "missing",
                {ErrorCode::NotFound, ErrorCode::InvalidArgument, ErrorCode::SecurityViolation}, "missing");
    expect_fail(f.ws / ".." / "ws2",
                {ErrorCode::SecurityViolation, ErrorCode::InvalidArgument, ErrorCode::NotFound}, "dotdot");
    write_file(f.ws / "file.txt", "x");
    expect_fail(f.ws / "file.txt", {ErrorCode::NotFound, ErrorCode::InvalidArgument},
                "a file, not a directory");
    EXPECT_EQ(f.ls->children.size(), 0u);
}

TEST(SessionCreate, WorkspaceMustNotContainTheAgentState) {
    Fixture f;
    f.settings.state_dir =
        f.ws / "agent-state";  // state inside the workspace would expose models.json and logs
    f.start();
    auto id = f.mgr->create(f.req());
    ASSERT_FALSE(id);
    EXPECT_EQ(id.error().code, ErrorCode::SecurityViolation);
}

TEST(SessionCreate, RejectsBadConfiguration) {
    {
        Fixture f;
        f.settings.base_url = "http://example.com/v1";  // a remote model endpoint is never allowed
        f.start();
        auto id = f.mgr->create(f.req());
        ASSERT_FALSE(id);
        EXPECT_EQ(id.error().code, ErrorCode::InvalidArgument);
    }
    {
        Fixture f;
        f.settings.token = "short";
        f.start();
        EXPECT_FALSE(f.mgr->create(f.req()));
    }
    {
        Fixture f;
        f.settings.model_id = "bad model id!";
        f.start();
        EXPECT_FALSE(f.mgr->create(f.req()));
    }
    {
        Fixture f;
        f.settings.agent_exe = f.tmp / "bin" / "missing.exe";
        f.start();
        auto id = f.mgr->create(f.req());
        ASSERT_FALSE(id);
        EXPECT_EQ(id.error().code, ErrorCode::NotFound);
    }
    {
        Fixture f;
        f.settings.kernel_python = fs::path();
        f.start();
        auto id = f.mgr->create(f.req());
        ASSERT_FALSE(id);
        EXPECT_EQ(id.error().code, ErrorCode::NotFound);
    }
    {
        Fixture f;
        f.settings.agent_exe = fs::path("relative-agent.exe");  // never launched: must be absolute
        f.start();
        EXPECT_FALSE(f.mgr->create(f.req()));
    }
}

// ================================================================================================================
// Launch plan, environment and secrets
// ================================================================================================================
TEST(SessionLaunch, ArgvEnvironmentAndLimitsComeFromTheLaunchPlan) {
    Fixture f;
    f.settings.limits.job_active_processes = 7;
    f.settings.limits.job_memory_bytes = 123456789;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok("please run the tests");
    f.wait_terminal(id);
    const process::LaunchSpec spec = f.ls->spec(0);

    EXPECT_EQ(spec.exe, f.exe);
    EXPECT_EQ(fs::weakly_canonical(spec.cwd), fs::weakly_canonical(f.ws));
    EXPECT_EQ(spec.active_process_limit, 7u);
    EXPECT_EQ(spec.memory_limit_bytes, 123456789u);

    const auto has = [&](const std::string& a) {
        return std::find(spec.args.begin(), spec.args.end(), a) != spec.args.end();
    };
    EXPECT_EQ(spec.args.front(), "--offline");
    for (const char* a : {"--provider", "hcs", "--model", "lfm3-omni-2b-q8_0", "--no-session", "--mode",
                          "json", "-p", "--system-prompt", "--cwd", "--"})
        EXPECT_TRUE(has(a)) << a;
    EXPECT_FALSE(has("--api-key")) << "the token must not travel on the command line";
    EXPECT_EQ(spec.args.back(), "please run the tests");
    for (const auto& a : spec.args)
        EXPECT_EQ(a.find(kToken), std::string::npos) << "token leaked into argv: " << a;

    EXPECT_EQ(spec.env.at("HCS_AGENT_TOKEN"), kToken);
    EXPECT_EQ(spec.env.at("PI_OFFLINE"), "1");
    EXPECT_EQ(fs::weakly_canonical(spec.env.at("PRIME_AGENT_CODING_AGENT_DIR")),
              fs::weakly_canonical(f.state / "agentdir"));
    EXPECT_EQ(spec.env.at("PRIME_AGENT_KERNEL_PYTHON"), f.py.string());
    EXPECT_NE(spec.env.at("TEMP").find(id), std::string::npos) << "TEMP is per session";
    EXPECT_NE(spec.env.at("HOME").find("state"), std::string::npos);
}

TEST(SessionLaunch, TheChildEnvironmentIsRebuiltFromAnAllowList) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) { c.finish(0); };
    const std::string id = f.create_ok();
    f.wait_terminal(id);
    const std::set<std::string> allowed = {"SystemRoot",
                                           "windir",
                                           "PATH",
                                           "PATHEXT",
                                           "COMSPEC",
                                           "NUMBER_OF_PROCESSORS",
                                           "PROCESSOR_ARCHITECTURE",
                                           "OS",
                                           "HOME",
                                           "USERPROFILE",
                                           "APPDATA",
                                           "LOCALAPPDATA",
                                           "TEMP",
                                           "TMP",
                                           "PRIME_AGENT_CODING_AGENT_DIR",
                                           "PRIME_AGENT_KERNEL_PYTHON",
                                           "PRIME_AGENT_TELEMETRY",
                                           "PI_OFFLINE",
                                           "DO_NOT_TRACK",
                                           "PYTHONUTF8",
                                           "HCS_AGENT_TOKEN"};
    for (const auto& [k, v] : f.ls->spec(0).env)
        EXPECT_TRUE(allowed.count(k)) << "unexpected variable in the child environment: " << k;
    // the home directory is the agent's own, not the real user profile
    EXPECT_EQ(fs::weakly_canonical(f.ls->spec(0).env.at("USERPROFILE")),
              fs::weakly_canonical(f.state / "home"));
    EXPECT_TRUE(fs::exists(f.state / "home"));
}

TEST(SessionLaunch, ModelsJsonOnDiskNamesTheVariableNeverTheToken) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line(kEnd);
        c.finish(0);
    };
    f.wait_terminal(f.create_ok());
    const std::string doc = slurp(f.state / "agentdir" / "models.json");
    ASSERT_FALSE(doc.empty());
    EXPECT_EQ(doc.find(kToken), std::string::npos);
    const json j = json::parse(doc);
    const json& p = j["providers"]["hcs"];
    EXPECT_EQ(p["apiKey"], "HCS_AGENT_TOKEN");
    EXPECT_EQ(p["baseUrl"], "http://127.0.0.1:8765/v1");
    EXPECT_EQ(p["models"][0]["id"], "lfm3-omni-2b-q8_0");
    EXPECT_EQ(p["models"][0]["contextWindow"], 5120);
    EXPECT_EQ(p["compat"]["supportsStore"], false);
}

TEST(SessionLaunch, LaunchFailureEndsTheSessionAsFailed) {
    Fixture f;
    f.ls->fail = true;
    f.start();
    const std::string id = f.create_ok();
    const SessionInfo info = f.wait_terminal(id);
    EXPECT_EQ(info.state, SessionState::Failed);
    EXPECT_NE(info.end_reason.find("launch failed"), std::string::npos);
    EXPECT_FALSE(info.exit_code.has_value());
    auto evs = f.all_events(id);
    ASSERT_FALSE(evs.empty());
    EXPECT_EQ(evs.back().kind, "session_end");
}

// ================================================================================================================
// Event stream
// ================================================================================================================
TEST(SessionEvents, HappyPathProducesOrderedBoundedEvents) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line("not json at all");
        c.line(tool_call("print(1)"));
        c.line(tool_end("1"));
        c.line(turn("all done"));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    const SessionInfo info = f.wait_terminal(id);
    EXPECT_EQ(info.state, SessionState::Completed);
    EXPECT_EQ(info.exit_code, 0);
    EXPECT_EQ(info.tool_calls, 1u);
    EXPECT_EQ(info.violations, 0u);
    EXPECT_GT(info.ended_ms, 0);

    const auto evs = f.all_events(id);
    EXPECT_EQ(kinds(evs),
              (std::vector<std::string>{"session_started", "agent_start", "tool_call", "tool_result",
                                        "assistant_text", "usage", "agent_end", "session_end"}));
    for (std::size_t i = 0; i < evs.size(); ++i)
        EXPECT_EQ(evs[i].seq, i + 1) << "seq must be contiguous from 1";
    EXPECT_EQ(evs[2].data["audit"], "ok");
    EXPECT_EQ(evs.back().data["state"], "completed");

    auto page = f.mgr->events(id, 0, 0, 1000);
    ASSERT_TRUE(page);
    EXPECT_TRUE(page.value().finished);
    EXPECT_EQ(page.value().next_seq, evs.size());
    EXPECT_EQ(f.mgr->list().size(), 1u);

    // persisted copy mirrors the stream
    const std::string persisted = slurp(f.state / "sessions" / id / "events.jsonl");
    EXPECT_EQ(static_cast<std::size_t>(std::count(persisted.begin(), persisted.end(), '\n')), evs.size());
}

TEST(SessionEvents, EventsPagingAfterAndMaxCount) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        for (int i = 0; i < 10; ++i) c.line(turn("t" + std::to_string(i)));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    f.wait_terminal(id);
    auto p1 = f.mgr->events(id, 0, 0, 4);
    ASSERT_TRUE(p1);
    EXPECT_EQ(p1.value().events.size(), 4u);
    EXPECT_FALSE(p1.value().finished);
    auto p2 = f.mgr->events(id, p1.value().next_seq, 0, 1000);
    ASSERT_TRUE(p2);
    EXPECT_EQ(p2.value().events.front().seq, 5u);
    EXPECT_TRUE(p2.value().finished);
    auto p3 = f.mgr->events(id, 10'000, 0, 10);  // far beyond the end
    ASSERT_TRUE(p3);
    EXPECT_TRUE(p3.value().events.empty());
    EXPECT_TRUE(p3.value().finished);
    auto p4 = f.mgr->events(id, 0, 0, 0);  // max_count 0 is clamped to 1, never an infinite or empty loop
    ASSERT_TRUE(p4);
    EXPECT_EQ(p4.value().events.size(), 1u);
}

TEST(SessionEvents, LongPollWakesWhenAnEventArrivesAndAtTheEnd) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild&) {};
    const std::string id = f.create_ok();
    auto child = f.ls->child(0);
    ASSERT_TRUE(child);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::atomic<bool> got{false};
    std::thread waiter([&] {
        // session_started is already there; wait for the one after it
        auto page = f.mgr->events(id, 1, 5000, 100);
        got = page && !page.value().events.empty();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_FALSE(got.load()) << "the poll should still be waiting";
    child->line(kStart);
    waiter.join();
    EXPECT_TRUE(got.load());
    child->line(kEnd);
    child->finish(0);
    f.wait_terminal(id);
    const auto t0 = std::chrono::steady_clock::now();
    auto done = f.mgr->events(id, 1000, 5000, 10);  // terminal: must not wait the full 5 s
    ASSERT_TRUE(done);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2));
}

TEST(SessionEvents, UnknownOrMalformedIdsAreNotFound) {
    Fixture f;
    f.start();
    for (const char* id : {"", "ag-", "ag-zzzzzzzzzzzzzzzz", "../../etc/passwd", "ag-0000000000000000",
                           "AG-0123456789abcdef", "ag-0123456789abcdef0", "ag-0123456789abcde\\x"}) {
        EXPECT_EQ(f.mgr->get(id).error().code, ErrorCode::NotFound) << id;
        EXPECT_EQ(f.mgr->events(id, 0, 0).error().code, ErrorCode::NotFound) << id;
        EXPECT_EQ(f.mgr->cancel(id).error().code, ErrorCode::NotFound) << id;
    }
}

TEST(SessionEvents, MalformedHostileOutputIsIgnoredNotFatal) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line("{");
        c.line(R"({"type":5})");
        c.line(R"({"type":"tool_execution_end","result":[1,2,3],"toolName":{}})");
        c.line(std::string("\xFF\xFE\x01\x02 binary junk"));
        c.line(R"({"type":"turn_end","message":{"role":"assistant","usage":{"input":"x"}}})");
        c.line(std::string(200, '['));
        c.out.push("\r\n\r\n");
        c.line(turn("still alive"));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    const SessionInfo info = f.wait_terminal(id);
    EXPECT_EQ(info.state, SessionState::Completed);
    EXPECT_TRUE(any_contains(f.all_events(id), "still alive"));
}

TEST(SessionEvents, OversizedOutputLineIsDroppedAndTheStreamRecovers) {
    Fixture f;
    f.settings.limits.max_line_bytes = 4096;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line(std::string(100000, 'j'));
        c.line(turn("after the big line"));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    f.wait_terminal(id);
    const auto evs = f.all_events(id);
    EXPECT_TRUE(any_contains(evs, "oversized output line was dropped"));
    EXPECT_TRUE(any_contains(evs, "after the big line"));
    EXPECT_FALSE(any_contains(evs, std::string(1000, 'j')));
}

TEST(SessionEvents, EventCountCapKeepsTheFinalEventAndSaysSo) {
    Fixture f;
    f.settings.limits.max_events = 20;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        for (int i = 0; i < 200; ++i) c.line(turn("event " + std::to_string(i)));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    f.wait_terminal(id);
    const auto evs = f.all_events(id);
    EXPECT_LE(evs.size(), 22u);
    EXPECT_EQ(count_kind(evs, "events_truncated"), 1u);
    EXPECT_EQ(evs.back().kind, "session_end") << "the final event is never dropped";
    EXPECT_EQ(f.wait_terminal(id).state, SessionState::Completed);
}

TEST(SessionEvents, TotalEventByteCapIsEnforced) {
    Fixture f;
    f.settings.limits.max_total_event_bytes = 4000;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        for (int i = 0; i < 100; ++i) c.line(turn(std::string(500, 'a' + (i % 26))));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    f.wait_terminal(id);
    const auto evs = f.all_events(id);
    std::size_t bytes = 0;
    for (const auto& e : evs) bytes += e.data.dump().size();
    EXPECT_LT(bytes, 4000u + 2000u) << "events must stop near the byte cap";
    EXPECT_EQ(count_kind(evs, "events_truncated"), 1u);
    EXPECT_EQ(evs.back().kind, "session_end");
}

TEST(SessionEvents, StderrBecomesAtMost200CappedLogEvents) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        std::string many;
        for (int i = 0; i < 500; ++i) many += "warning " + std::to_string(i) + "\r\n";
        many += std::string(20000, 'q') + "\n";  // one very long line
        c.err.push(many);
        c.line(kStart);
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    f.wait_terminal(id);
    const auto evs = f.all_events(id);
    std::size_t stderr_logs = 0;
    for (const auto& e : evs)
        if (e.kind == "log" && e.data.value("stream", "") == "stderr") {
            ++stderr_logs;
            EXPECT_LE(e.data["text"].get<std::string>().size(), 2048u);
        }
    EXPECT_LE(stderr_logs, 200u);
    EXPECT_GE(stderr_logs, 1u);
}

// ================================================================================================================
// Secrets never reach events, logs, disk
// ================================================================================================================
TEST(SessionSecrets, TheTokenIsRedactedFromEventsLogsAndPersistedFiles) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.err.push(std::string("leaking ") + kToken + "\n");
        c.line(kStart);
        c.line(tool_call(std::string("print('") + kToken + "')"));
        c.line(tool_end(std::string("value=") + kToken));
        c.line(turn(std::string("the token is ") + kToken + " and sk-abcdefghijklmnopqrstuvwx"));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok(std::string("my token is ") + kToken);
    f.wait_terminal(id);
    const auto evs = f.all_events(id);
    EXPECT_FALSE(any_contains(evs, kToken));
    EXPECT_FALSE(any_contains(evs, "sk-abcdefghijklmnopqrstuvwx"));
    EXPECT_TRUE(any_contains(evs, "[redacted]"));
    EXPECT_EQ(f.log_stream.str().find(kToken), std::string::npos) << "token in the log";
    EXPECT_EQ(slurp(f.state / "sessions" / id / "events.jsonl").find(kToken), std::string::npos);
    EXPECT_EQ(slurp(f.state / "agentdir" / "models.json").find(kToken), std::string::npos);
    auto info = f.mgr->get(id);
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().task_preview.find(kToken), std::string::npos);
    EXPECT_EQ(to_json(info.value()).dump().find(kToken), std::string::npos);
}

TEST(SessionSecrets, TaskPreviewIsCappedOnAUtf8Boundary) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) { c.finish(0); };
    std::string task;
    for (int i = 0; i < 100; ++i) task += "\xE2\x82\xAC";  // 3-byte euro signs, 300 bytes
    const std::string id = f.create_ok(task);
    f.wait_terminal(id);
    const std::string preview = f.mgr->get(id).value().task_preview;
    EXPECT_LE(preview.size(), 120u);
    EXPECT_EQ(preview.size() % 3, 0u) << "the cut must not split a code point";
}

TEST(SessionSecrets, ToolCallLogsCarryMetadataNotCode) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line(tool_call("print('very-private-source-code')"));
        c.line(kEnd);
        c.finish(0);
    };
    f.wait_terminal(f.create_ok());
    EXPECT_EQ(f.log_stream.str().find("very-private-source-code"), std::string::npos);
    EXPECT_NE(f.log_stream.str().find("tool_call"), std::string::npos);
}

// ================================================================================================================
// Policy, timeouts, cancel, concurrency, shutdown
// ================================================================================================================
TEST(SessionPolicy, ABrokenToolCallKillsTheTreeAndEndsPolicyKilled) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line(tool_call("import socket"));
    };
    const std::string id = f.create_ok();
    const SessionInfo info = f.wait_terminal(id);
    EXPECT_EQ(info.state, SessionState::PolicyKilled);
    EXPECT_EQ(info.violations, 1u);
    auto child = f.ls->child(0);
    ASSERT_TRUE(child);
    EXPECT_GE(child->terminate_calls.load(), 1);
    const auto evs = f.all_events(id);
    EXPECT_EQ(count_kind(evs, "policy_violation"), 1u);
    for (const auto& e : evs)
        if (e.kind == "policy_violation") {
            EXPECT_EQ(e.data["rule"], "network");
            EXPECT_EQ(e.data["enforced"], true);
        }
}

TEST(SessionPolicy, UnknownToolAndDynamicCodeAndPathEscapeAreAllViolations) {
    struct Case {
        std::string tool;
        std::string code;
        std::string rule;
    };
    const std::vector<Case> cases = {{"bash", "ls", "unknown_tool"},
                                     {"python_repl", "exec('1')", "dynamic_code"},
                                     {"python_repl", "open('/etc/passwd')", "path_escape"},
                                     {"python_repl", "bash('curl http://x')", ""}};
    for (const auto& c : cases) {
        Fixture f;
        f.start();
        f.ls->on_launch = [&](FakeChild& ch) {
            ch.line(kStart);
            ch.line(tool_call(c.code, c.tool));
        };
        const std::string id = f.create_ok();
        EXPECT_EQ(f.wait_terminal(id).state, SessionState::PolicyKilled) << c.code;
        for (const auto& e : f.all_events(id))
            if (e.kind == "policy_violation" && !c.rule.empty())
                EXPECT_EQ(e.data["rule"], c.rule) << c.code;
    }
}

TEST(SessionPolicy, WithoutEnforcementViolationsAreRecordedButNothingIsKilled) {
    Fixture f;
    f.settings.enforce_audit = false;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line(tool_call("import socket"));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    const SessionInfo info = f.wait_terminal(id);
    EXPECT_EQ(info.state, SessionState::Completed);
    EXPECT_EQ(info.violations, 1u);
    auto child = f.ls->child(0);
    ASSERT_TRUE(child);
    EXPECT_EQ(child->terminate_calls.load(), 1) << "only the final reap, never a policy kill";
    for (const auto& e : f.all_events(id))
        if (e.kind == "policy_violation")
            EXPECT_EQ(e.data["enforced"], false);
}

TEST(SessionPolicy, AllowedToolCallsAreNotViolations) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line(tool_call("r = bash('git status')\nprint(open('src/a.py').read())"));
        c.line(kEnd);
        c.finish(0);
    };
    const std::string id = f.create_ok();
    const SessionInfo info = f.wait_terminal(id);
    EXPECT_EQ(info.state, SessionState::Completed);
    EXPECT_EQ(info.violations, 0u);
}

TEST(SessionExit, OutcomeDependsOnExitCodeAndAgentEnd) {
    struct Case {
        std::vector<std::string> lines;
        int code;
        SessionState state;
    };
    const std::vector<Case> cases = {{{kStart, kEnd}, 0, SessionState::Completed},
                                     {{kStart, kEnd}, 2, SessionState::Failed},
                                     {{kStart}, 0, SessionState::Failed},
                                     {{}, 0, SessionState::Failed},
                                     {{kStart}, 3, SessionState::Failed}};
    for (const auto& c : cases) {
        Fixture f;
        f.start();
        f.ls->on_launch = [&](FakeChild& ch) {
            for (const auto& l : c.lines) ch.line(l);
            ch.finish(c.code);
        };
        const std::string id = f.create_ok();
        const SessionInfo info = f.wait_terminal(id);
        EXPECT_EQ(info.state, c.state) << "code " << c.code << ", " << c.lines.size() << " lines";
        if (c.state == SessionState::Failed)
            EXPECT_FALSE(info.end_reason.empty());
    }
}

TEST(SessionTimeout, TheWatchdogKillsAHungAgent) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) { c.line(kStart); };  // then silence
    const std::string id = f.create_ok("hang", 300u);
    const auto t0 = std::chrono::steady_clock::now();
    const SessionInfo info = f.wait_terminal(id, 8000);
    EXPECT_EQ(info.state, SessionState::TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(6));
    EXPECT_GE(f.ls->child(0)->terminate_calls.load(), 1);
}

TEST(SessionTimeout, RequestedTimeoutIsClampedToTheConfiguredMaximum) {
    Fixture f;
    f.settings.limits.max_timeout_ms = 300;
    f.start();
    f.ls->on_launch = [](FakeChild& c) { c.line(kStart); };
    const std::string id = f.create_ok("hang", 3'600'000u);  // an hour requested, 300 ms allowed
    EXPECT_EQ(f.wait_terminal(id, 8000).state, SessionState::TimedOut);
}

TEST(SessionCancel, CancelKillsTheChildAndIsOnlyValidWhileRunning) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) { c.line(kStart); };
    const std::string id = f.create_ok();
    ASSERT_TRUE(f.ls->child(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(f.mgr->cancel(id));
    const SessionInfo info = f.wait_terminal(id);
    EXPECT_EQ(info.state, SessionState::Cancelled);
    EXPECT_GE(f.ls->child(0)->terminate_calls.load(), 1);
    auto again = f.mgr->cancel(id);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(f.all_events(id).back().data["state"], "cancelled");
}

TEST(SessionCancel, CancelRightAfterCreateStillEndsTheSession) {
    for (int i = 0; i < 20; ++i) {
        Fixture f;
        f.start();
        f.ls->on_launch = [](FakeChild& c) { c.line(kStart); };
        const std::string id = f.create_ok();
        (void)f.mgr->cancel(id);  // may race with launch; either way the session must end
        const SessionInfo info = f.wait_terminal(id, 8000);
        EXPECT_TRUE(info.state == SessionState::Cancelled || info.state == SessionState::Failed)
            << to_string(info.state);
    }
}

TEST(SessionConcurrency, OnlyOneSessionRunsAtATime) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) { c.line(kStart); };
    const std::string first = f.create_ok();
    auto second = f.mgr->create(f.req());
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, ErrorCode::Busy);
    ASSERT_TRUE(f.mgr->cancel(first));
    f.wait_terminal(first);
    f.ls->on_launch = [](FakeChild& c) { c.finish(0); };
    auto third = f.mgr->create(f.req());
    EXPECT_TRUE(third) << (third ? "" : third.error().message);
}

TEST(SessionConcurrency, FinishedSessionsAreEvictedBeyondTheCap) {
    Fixture f;
    f.settings.limits.max_sessions_kept = 3;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        c.line(kEnd);
        c.finish(0);
    };
    std::vector<std::string> ids;
    for (int i = 0; i < 6; ++i) {
        ids.push_back(f.create_ok("task " + std::to_string(i)));
        f.wait_terminal(ids.back());
    }
    EXPECT_LE(f.mgr->list().size(), 3u);
    EXPECT_EQ(f.mgr->get(ids.front()).error().code, ErrorCode::NotFound);
    EXPECT_TRUE(f.mgr->get(ids.back()));
}

TEST(SessionConcurrency, ParallelReadersAndCancelNeverDeadlockOrCrash) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) {
        c.line(kStart);
        for (int i = 0; i < 300; ++i) c.line(turn("t" + std::to_string(i)));
    };
    const std::string id = f.create_ok();
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            std::uint64_t next = 0;
            while (!stop) {
                (void)f.mgr->list();
                (void)f.mgr->get(id);
                auto p = f.mgr->events(id, next, 20, 50);
                if (p)
                    next = p.value().next_seq;
            }
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(f.mgr->cancel(id));
    f.wait_terminal(id);
    stop = true;
    for (auto& t : threads) t.join();
    EXPECT_EQ(f.mgr->get(id).value().state, SessionState::Cancelled);
}

TEST(SessionShutdown, ShutdownCancelsRunningSessionsAndRefusesNewOnes) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) { c.line(kStart); };
    const std::string id = f.create_ok();
    ASSERT_TRUE(f.ls->child(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    f.mgr->shutdown();
    EXPECT_GE(f.ls->child(0)->terminate_calls.load(), 1);
    EXPECT_TRUE(is_terminal(f.mgr->get(id).value().state));
    auto late = f.mgr->create(f.req());
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error().code, ErrorCode::InvalidState);
    f.mgr->shutdown();  // idempotent
}

TEST(SessionShutdown, DestroyingTheManagerWhileRunningEndsTheChild) {
    Fixture f;
    f.start();
    f.ls->on_launch = [](FakeChild& c) { c.line(kStart); };
    f.create_ok();
    ASSERT_TRUE(f.ls->child(0));
    f.mgr.reset();  // destructor must join the runner threads without hanging
    EXPECT_GE(f.ls->child(0)->terminate_calls.load(), 1);
}

TEST(SessionJson, InfoSerialisesWithoutEndTimeWhileRunning) {
    SessionInfo s;
    s.id = "ag-0123456789abcdef";
    s.state = SessionState::Running;
    json j = to_json(s);
    EXPECT_EQ(j["state"], "running");
    EXPECT_FALSE(j.contains("ended_ms"));
    EXPECT_TRUE(j["exit_code"].is_null());
    s.state = SessionState::Completed;
    s.exit_code = 0;
    j = to_json(s);
    EXPECT_TRUE(j.contains("ended_ms"));
    EXPECT_EQ(j["exit_code"], 0);
    EXPECT_EQ(to_string(SessionState::PolicyKilled), "policy_killed");
    EXPECT_TRUE(default_agent_system_prompt().find("python_repl") != std::string::npos);
}

// ================================================================================================================
// Real launcher + real helper executable standing in for prime-agent
// ================================================================================================================
#ifdef _WIN32
namespace {

fs::path probe_path() {
    return fs::path(std::string(HCS_CHILD_PROBE) + "/hcs_child_probe.exe");
}

struct RealFixture : Fixture {
    RealFixture() {
        settings.agent_exe = probe_path();
        settings.kernel_python = probe_path();
        settings.limits.kill_grace_ms = 2000;
        start_with(process::make_native_launcher());
    }
};

}  // namespace

TEST(SessionReal, ScriptedAgentCompletesThroughTheJobObjectLauncher) {
    RealFixture f;
    const std::string id = f.create_ok("ok");
    const SessionInfo info = f.wait_terminal(id, 20000);
    EXPECT_EQ(info.state, SessionState::Completed) << info.end_reason;
    EXPECT_EQ(info.exit_code, 0);
    const auto evs = f.all_events(id);
    EXPECT_EQ(count_kind(evs, "tool_call"), 1u);
    EXPECT_EQ(count_kind(evs, "tool_result"), 1u);
    EXPECT_TRUE(any_contains(evs, "all done"));
    EXPECT_TRUE(any_contains(evs, "warming up")) << "stderr arrives on its own pipe as a log event";
    EXPECT_FALSE(any_contains(evs, "this line is not json"));
}

TEST(SessionReal, TheChildSeesTheTokenOnlyInItsEnvironmentAndNothingFromTheParent) {
    ASSERT_EQ(_putenv_s("HCS_TEST_PARENT_SECRET3", "must-not-leak"), 0);
    RealFixture f;
    const std::string id = f.create_ok("envreport");
    const SessionInfo info = f.wait_terminal(id, 20000);
    ASSERT_EQ(info.state, SessionState::Completed) << info.end_reason;
    std::string report;
    for (const auto& e : f.all_events(id))
        if (e.kind == "assistant_text")
            report = e.data["text"].get<std::string>();
    ASSERT_FALSE(report.empty());
    EXPECT_NE(report.find("token_in_env=1"), std::string::npos) << report;
    EXPECT_NE(report.find("token_in_argv=0"), std::string::npos) << report;
    EXPECT_NE(report.find("token_in_other_var=0"), std::string::npos) << report;
    EXPECT_NE(report.find("HCS_AGENT_TOKEN"), std::string::npos);
    EXPECT_NE(report.find("PI_OFFLINE"), std::string::npos);
    EXPECT_EQ(report.find("HCS_TEST_PARENT_SECRET3"), std::string::npos) << report;
    EXPECT_EQ(report.find("USERNAME"), std::string::npos) << report;
    EXPECT_EQ(report.find("COMPUTERNAME"), std::string::npos) << report;
    EXPECT_FALSE(any_contains(f.all_events(id), kToken));
}

TEST(SessionReal, AnEchoedTokenIsRedactedEvenWhenTheChildPrintsIt) {
    RealFixture f;
    const std::string id = f.create_ok("echotoken");
    f.wait_terminal(id, 20000);
    const auto evs = f.all_events(id);
    EXPECT_FALSE(any_contains(evs, kToken));
    EXPECT_TRUE(any_contains(evs, "the token is [redacted]"));
    EXPECT_EQ(f.log_stream.str().find(kToken), std::string::npos);
}

TEST(SessionReal, APolicyViolationKillsTheRealProcessPromptly) {
    RealFixture f;
    const std::string id = f.create_ok("violation");  // the child would sleep 30 s
    const auto t0 = std::chrono::steady_clock::now();
    const SessionInfo info = f.wait_terminal(id, 15000);
    EXPECT_EQ(info.state, SessionState::PolicyKilled);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10)) << "the tree was not killed";
}

TEST(SessionReal, WallClockTimeoutKillsTheRealProcess) {
    RealFixture f;
    const std::string id = f.create_ok("hang", 600u);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(f.wait_terminal(id, 15000).state, SessionState::TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));
}

TEST(SessionReal, CancelKillsTheWholeTreeIncludingAGrandchild) {
    RealFixture f;
    const fs::path marker = f.tmp / "grandchild-marker.txt";
    const std::string id = f.create_ok("spawn:" + marker.string());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(f.mgr->cancel(id));
    EXPECT_EQ(f.wait_terminal(id, 15000).state, SessionState::Cancelled);
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    EXPECT_FALSE(fs::exists(marker)) << "the grandchild survived the cancel";
}

TEST(SessionReal, EventAndLineCapsHoldAgainstARealFloodingProcess) {
    {
        RealFixture f;
        f.settings.limits.max_events = 50;
        f.start_with(process::make_native_launcher());
        const std::string id = f.create_ok("flood");
        EXPECT_EQ(f.wait_terminal(id, 30000).state, SessionState::Completed);
        const auto evs = f.all_events(id);
        EXPECT_LE(evs.size(), 52u);
        EXPECT_EQ(count_kind(evs, "events_truncated"), 1u);
        EXPECT_EQ(evs.back().kind, "session_end");
    }
    {
        RealFixture f;
        f.settings.limits.max_line_bytes = 64 * 1024;
        f.start_with(process::make_native_launcher());
        const std::string id = f.create_ok("bigline");
        EXPECT_EQ(f.wait_terminal(id, 30000).state, SessionState::Completed);
        const auto evs = f.all_events(id);
        EXPECT_TRUE(any_contains(evs, "oversized output line was dropped"));
        EXPECT_TRUE(any_contains(evs, "after the big line"));
    }
}

TEST(SessionReal, ExitCodesMapToStates) {
    RealFixture f;
    {
        const std::string id = f.create_ok("exit3");
        const SessionInfo info = f.wait_terminal(id, 20000);
        EXPECT_EQ(info.state, SessionState::Failed);
        EXPECT_EQ(info.exit_code, 3);
    }
    {
        const std::string id = f.create_ok("noend");
        EXPECT_EQ(f.wait_terminal(id, 20000).state, SessionState::Failed);
    }
}

TEST(SessionReal, ShutdownWithARunningRealChildLeavesNothingBehind) {
    RealFixture f;
    const fs::path marker = f.tmp / "shutdown-marker.txt";
    const std::string id = f.create_ok("spawn:" + marker.string());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    f.mgr.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    EXPECT_FALSE(fs::exists(marker));
    (void)id;
}
#endif
