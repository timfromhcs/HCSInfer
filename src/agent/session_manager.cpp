#include "hcs/agent/session_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>

#include "hcs/agent/agent_redact.hpp"
#include "hcs/agent/event_parser.hpp"
#include "hcs/fs_util.hpp"

namespace hcs::agent {

namespace fs = std::filesystem;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string getenv_str(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf)
        return {};
    std::string v(buf);
    std::free(buf);
    return v;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

bool valid_id(const std::string& id) {
    if (id.size() != 19 || id.rfind("ag-", 0) != 0)
        return false;
    return std::all_of(id.begin() + 3, id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

}  // namespace

std::string_view to_string(SessionState s) noexcept {
    switch (s) {
        case SessionState::Starting:
            return "starting";
        case SessionState::Running:
            return "running";
        case SessionState::Completed:
            return "completed";
        case SessionState::Failed:
            return "failed";
        case SessionState::Cancelled:
            return "cancelled";
        case SessionState::TimedOut:
            return "timed_out";
        case SessionState::PolicyKilled:
            return "policy_killed";
    }
    return "unknown";
}

std::string default_agent_system_prompt() {
    return "You are HCS Agent, a small coding agent. You work only inside the current directory. You have "
           "ONE tool, "
           "python_repl, which runs Python code.\n"
           "- Run a shell command: r = await bash('python -m unittest'); print(r.output)\n"
           "- Read a file: print(open('stats.py').read())\n"
           "- Change a file: await edit.run('stats.py', 'exact old text', 'exact new text')\n"
           "Work in small steps: run the tests, read the failing code, fix it, run the tests again. When the "
           "tests "
           "pass, reply with a one-line summary and stop. Never use the network and never touch files "
           "outside the "
           "current directory.";
}

json to_json(const SessionInfo& s) {
    json j = {{"id", s.id},
              {"state", std::string(to_string(s.state))},
              {"root", s.root},
              {"task_preview", s.task_preview},
              {"created_ms", s.created_ms},
              {"event_count", s.event_count},
              {"tool_calls", s.tool_calls},
              {"policy_violations", s.violations},
              {"end_reason", s.end_reason}};
    if (is_terminal(s.state))
        j["ended_ms"] = s.ended_ms;
    j["exit_code"] = s.exit_code ? json(*s.exit_code) : json(nullptr);
    return j;
}

json to_json(const AgentEvent& e) {
    return {{"seq", e.seq}, {"ts_ms", e.ts_ms}, {"kind", e.kind}, {"data", e.data}};
}

struct SessionManager::Session {
    std::string id;
    std::string task;
    fs::path root;
    fs::path dir;
    mutable std::mutex m;
    std::condition_variable cv;
    SessionInfo info;
    std::vector<AgentEvent> events;
    std::size_t bytes = 0;
    bool truncated_noted = false;
    LaunchPlan plan;  // from build_launch_plan: program, argv prefix, canonical cwd, base environment
    std::shared_ptr<process::IChildProcess> child;
    std::atomic<bool> cancel_req{false};
    std::atomic<bool> violation{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> killed{false};
    Clock::time_point deadline{};
    std::unique_ptr<ToolAudit> audit;
    std::thread runner;
    std::ofstream log_file;
    std::uint32_t log_events = 0;
    bool saw_end = false;
};

SessionManager::SessionManager(AgentSettings settings, PathGuard approved_roots,
                               std::unique_ptr<process::IProcessLauncher> launcher, Logger& logger)
    : settings_(std::move(settings)),
      roots_(std::move(approved_roots)),
      launcher_(std::move(launcher)),
      log_(logger) {
    std::random_device rd;
    rng_state_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    if (settings_.system_prompt.empty())
        settings_.system_prompt = default_agent_system_prompt();
    watchdog_ = std::thread([this] { watchdog(); });
}

SessionManager::~SessionManager() {
    shutdown();
}

std::string SessionManager::new_id() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id = "ag-";
    for (int i = 0; i < 16; ++i) {
        rng_state_ = rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        id.push_back(kHex[(rng_state_ >> 33) & 0xF]);
    }
    return id;
}

std::shared_ptr<SessionManager::Session> SessionManager::find(const std::string& id) const {
    std::lock_guard lk(mu_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

namespace {
constexpr const char* kTokenEnv = "HCS_AGENT_TOKEN";
}

ProviderConfig SessionManager::provider_config() const {
    ProviderConfig p;
    p.name = "hcs";
    p.base_url = settings_.base_url;
    p.api_key = settings_.token;
    p.api_key_env =
        kTokenEnv;  // models.json holds the variable NAME; the token reaches the child via env only
    p.local_server_compat = true;
    p.models = {ProviderModel{settings_.model_id,
                              static_cast<int>(settings_.context_window),
                              static_cast<int>(settings_.max_tokens),
                              {"text"}}};
    return p;
}

Result<void> SessionManager::write_models_json() const {
    auto doc = provider_config_json(provider_config());
    if (!doc)
        return doc.error();
    return atomic_write_file(agent_dir() / "models.json", doc.value() + "\n");
}

Result<std::map<std::string, std::string>> SessionManager::build_env(const fs::path& session_dir) const {
    std::map<std::string, std::string> env;
    auto add = [&](const std::string& k, const std::string& v) { env[k] = v; };
    for (const char* k : {"SystemRoot", "windir", "PATH", "PATHEXT", "COMSPEC", "NUMBER_OF_PROCESSORS",
                          "PROCESSOR_ARCHITECTURE", "OS"}) {
        const std::string v = getenv_str(k);
        if (!v.empty())
            add(k, v);
    }
    const fs::path home = settings_.state_dir / "home";
    const fs::path tmp = session_dir / "tmp";
    std::error_code ec;
    fs::create_directories(home, ec);
    fs::create_directories(tmp, ec);
    if (ec)
        return make_error(ErrorCode::IoError, "cannot create agent home/tmp directories");
    add("HOME", home.string());
    add("USERPROFILE", home.string());
    add("APPDATA", (home / "AppData" / "Roaming").string());
    add("LOCALAPPDATA", (home / "AppData" / "Local").string());
    add("TEMP", tmp.string());
    add("TMP", tmp.string());
    add("PRIME_AGENT_KERNEL_PYTHON", settings_.kernel_python.string());
    add("DO_NOT_TRACK", "1");
    add("PRIME_AGENT_TELEMETRY", "0");
    add("PYTHONUTF8", "1");
    // PRIME_AGENT_CODING_AGENT_DIR, PI_OFFLINE and the token variable come from build_launch_plan
    // (run_session).
    return env;
}

void SessionManager::push(Session& s, std::string kind, json data) {
    const std::vector<std::string> secrets{settings_.token};
    std::string dumped = data.dump();
    const std::string red = redact_secrets(dumped, secrets);
    if (red != dumped)
        data = json::parse(red, nullptr, false).is_discarded() ? json{{"redacted", true}} : json::parse(red);
    const bool final_event = kind == "session_end";
    std::lock_guard lk(s.m);
    const auto& lim = settings_.limits;
    if (!final_event &&
        (s.events.size() + 1 >= lim.max_events || s.bytes + red.size() > lim.max_total_event_bytes)) {
        if (!s.truncated_noted) {
            s.truncated_noted = true;
            AgentEvent n;
            n.seq = s.events.size() + 1;
            n.ts_ms = unix_ms();
            n.kind = "events_truncated";
            n.data = {{"reason", "event cap reached; later events are dropped"}};
            s.events.push_back(std::move(n));
        }
        s.cv.notify_all();
        return;
    }
    AgentEvent e;
    e.seq = s.events.size() + 1;
    e.ts_ms = unix_ms();
    e.kind = std::move(kind);
    e.data = std::move(data);
    s.bytes += red.size();
    if (s.log_file.is_open())
        s.log_file << to_json(e).dump() << '\n' << std::flush;
    s.info.event_count = e.seq;
    s.events.push_back(std::move(e));
    s.cv.notify_all();
}

void SessionManager::finish(Session& s, SessionState state, std::optional<int> exit_code,
                            std::string reason) {
    {
        std::lock_guard lk(s.m);
        if (is_terminal(s.info.state))
            return;
    }
    push(s, "session_end",
         {{"state", std::string(to_string(state))},
          {"exit_code", exit_code ? json(*exit_code) : json(nullptr)},
          {"reason", reason}});
    std::lock_guard lk(s.m);
    s.info.state = state;
    s.info.exit_code = exit_code;
    s.info.end_reason = std::move(reason);
    s.info.ended_ms = unix_ms();
    s.cv.notify_all();
}

Result<std::string> SessionManager::create(const CreateRequest& req) {
    if (req.task.empty() || req.task.size() > settings_.limits.max_task_bytes)
        return make_error(ErrorCode::InvalidArgument,
                          "task must be 1.." + std::to_string(settings_.limits.max_task_bytes) + " bytes");
    if (req.task.find('\0') != std::string::npos)
        return make_error(ErrorCode::InvalidArgument, "task contains NUL");
    if (req.timeout_ms && *req.timeout_ms == 0)
        return make_error(ErrorCode::InvalidArgument, "timeout_ms must be greater than zero");
    if (settings_.agent_exe.empty() || !fs::exists(settings_.agent_exe))
        return make_error(ErrorCode::NotFound, "agent executable is not configured or missing");
    if (settings_.kernel_python.empty() || !fs::exists(settings_.kernel_python))
        return make_error(
            ErrorCode::NotFound,
            "agent kernel Python is not prepared (see docs/agent.md 'Prepare the kernel venv')");
    if (settings_.token.size() < 16 || settings_.base_url.empty() || settings_.model_id.empty())
        return make_error(ErrorCode::InvalidArgument,
                          "agent endpoint (base_url, token, model) is not configured");
    if (req.root.empty() || !req.root.is_absolute())
        return make_error(ErrorCode::InvalidArgument, "root must be an absolute path");
    auto root = roots_.resolve(req.root);
    if (!root)
        return root.error();
    std::error_code ec;
    if (!fs::is_directory(root.value(), ec))
        return make_error(ErrorCode::NotFound, "root is not an existing directory");
    // The workspace must never contain the agent's own state (it would expose models.json, logs, other
    // sessions).
    const fs::path state = fs::weakly_canonical(settings_.state_dir, ec);
    const fs::path& r = root.value();
    if (!ec && (state == r || (state.native().size() > r.native().size() &&
                               state.native().compare(0, r.native().size(), r.native()) == 0 &&
                               (state.native()[r.native().size()] == fs::path::preferred_separator))))
        return make_error(ErrorCode::SecurityViolation, "root must not contain the agent state directory");
    if (r == r.root_path())
        return make_error(ErrorCode::SecurityViolation, "a drive root is not an acceptable workspace");

    auto audit = ToolAudit::create(r, settings_.audit);
    if (!audit)
        return audit.error();

    // The launch plan validates the program path, the loopback-only provider, the model and that the
    // workspace resolves inside an approved root; a bad configuration fails here, synchronously, not inside
    // the runner.
    LaunchRequest launch;
    launch.program = settings_.agent_exe;
    launch.workspaces = roots_;
    launch.workspace = r;
    launch.agent_dir = agent_dir();
    launch.provider = provider_config();
    launch.model_id = settings_.model_id;
    auto plan = build_launch_plan(launch);
    if (!plan)
        return plan.error();

    const std::uint32_t timeout = std::min(req.timeout_ms.value_or(settings_.limits.default_timeout_ms),
                                           settings_.limits.max_timeout_ms);

    auto s = std::make_shared<Session>();
    {
        std::lock_guard lk(mu_);
        if (stopping_)
            return make_error(ErrorCode::InvalidState, "agent manager is shutting down");
        std::uint32_t active = 0;
        for (const auto& [id, sess] : sessions_) {
            std::lock_guard lk2(sess->m);
            if (!is_terminal(sess->info.state))
                ++active;
        }
        if (active >= settings_.limits.max_concurrent)
            return make_error(ErrorCode::Busy, "an agent session is already running");
        s->id = new_id();
        // Evict the oldest finished sessions beyond the cap.
        while (sessions_.size() >= settings_.limits.max_sessions_kept) {
            auto victim = std::find_if(order_.begin(), order_.end(), [&](const std::string& id) {
                auto& v = sessions_.at(id);
                std::lock_guard lk2(v->m);
                return is_terminal(v->info.state);
            });
            if (victim == order_.end())
                break;
            auto v = sessions_.at(*victim);
            if (v->runner.joinable())
                v->runner.join();
            sessions_.erase(*victim);
            order_.erase(victim);
        }
        sessions_[s->id] = s;
        order_.push_back(s->id);
    }
    s->plan = std::move(plan).value();
    s->root = s->plan.cwd;
    s->task = req.task;
    s->dir = settings_.state_dir / "sessions" / s->id;
    s->audit = std::make_unique<ToolAudit>(std::move(audit).value());
    s->info.id = s->id;
    s->info.state = SessionState::Starting;
    s->info.root = r.string();
    bool tr = false;
    s->info.task_preview = redact_secrets(cap_utf8(req.task, 120, tr), {settings_.token});
    s->info.created_ms = unix_ms();
    s->deadline = Clock::now() + std::chrono::milliseconds(timeout);
    fs::create_directories(s->dir, ec);
    if (settings_.persist_events && !ec)
        s->log_file.open(s->dir / "events.jsonl", std::ios::binary | std::ios::app);
    if (auto w = write_models_json(); !w) {
        finish(*s, SessionState::Failed, std::nullopt, "cannot write agent provider config");
        return w.error();
    }
    log_.info("agent", "session created", {{"session", s->id}, {"timeout_ms", std::to_string(timeout)}});
    s->runner = std::thread([this, s, timeout] { run_session(s, timeout); });
    return s->id;
}

void SessionManager::run_session(const std::shared_ptr<Session>& s, std::uint32_t timeout_ms) {
    (void)timeout_ms;  // the deadline was fixed at creation; the watchdog enforces it
    const auto& lim = settings_.limits;
    auto env = build_env(s->dir);
    if (!env) {
        finish(*s, SessionState::Failed, std::nullopt, env.error().message);
        return;
    }
    process::LaunchSpec spec;
    spec.exe = s->plan.program;
    spec.cwd = s->plan.cwd;
    spec.env = std::move(env).value();
    for (const auto& [k, v] : s->plan.env)  // agent dir, PI_OFFLINE and the token variable
        spec.env[k] = v;
    spec.active_process_limit = lim.job_active_processes;
    spec.memory_limit_bytes = lim.job_memory_bytes;
    spec.args = s->plan.args;
    for (const char* a : {"--no-session", "--mode", "json", "-p", "--system-prompt"})
        spec.args.emplace_back(a);
    spec.args.push_back(settings_.system_prompt);
    spec.args.emplace_back("--cwd");
    spec.args.push_back(s->plan.cwd.string());
    spec.args.emplace_back("--");
    spec.args.push_back(s->task);
    auto launched = launcher_->launch(spec);
    if (!launched) {
        finish(*s, SessionState::Failed, std::nullopt, "launch failed: " + launched.error().message);
        return;
    }
    std::shared_ptr<process::IChildProcess> child(std::move(launched).value());
    {
        std::lock_guard lk(s->m);
        s->child = child;
        s->info.state = SessionState::Running;
    }
    push(*s, "session_started",
         {{"root", s->root.string()}, {"model", settings_.model_id}, {"pid", child->pid()}});
    log_.info("agent", "session started", {{"session", s->id}, {"pid", std::to_string(child->pid())}});
    if (s->cancel_req.load())
        child->terminate();

    std::thread err_thread([&] {
        std::string pending;
        char buf[4096];
        while (true) {
            const std::size_t n = child->read_stderr(buf, sizeof(buf));
            if (n == 0)
                break;
            pending.append(buf, n);
            std::size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || s->log_events >= 200)
                    continue;
                ++s->log_events;
                bool t = false;
                push(*s, "log", {{"stream", "stderr"}, {"text", cap_utf8(line, 2048, t)}});
            }
            if (pending.size() > lim.max_line_bytes)
                pending.clear();
        }
    });

    std::string pending;
    bool discarding = false;
    char buf[8192];
    auto handle_line = [&](const std::string& line) {
        for (auto& ev : parse_agent_line(line, settings_.audit.max_code_bytes + 1)) {
            if (ev.kind == "tool_call") {
                std::uint32_t n;
                {
                    std::lock_guard lk(s->m);
                    n = ++s->info.tool_calls;
                }
                const AuditVerdict v = s->audit->audit_tool_call(ev.tool, ev.payload);
                bool t = false;
                const std::string shown = cap_utf8(ev.payload, lim.max_event_text_bytes, t);
                ev.data["text"] = shown;
                ev.data["truncated"] = t;
                ev.data["audit"] = v.rule;
                log_.info("agent", "tool_call",
                          {{"session", s->id},
                           {"n", std::to_string(n)},
                           {"tool", ev.tool.substr(0, 64)},
                           {"bytes", std::to_string(ev.payload.size())},
                           {"audit", v.rule}});
                push(*s, "tool_call", ev.data);
                if (!v.allowed) {
                    {
                        std::lock_guard lk(s->m);
                        ++s->info.violations;
                    }
                    push(*s, "policy_violation",
                         {{"rule", v.rule}, {"detail", v.detail}, {"enforced", settings_.enforce_audit}});
                    log_.warn("agent", "policy violation",
                              {{"session", s->id},
                               {"rule", v.rule},
                               {"enforced", settings_.enforce_audit ? "true" : "false"}});
                    if (settings_.enforce_audit) {
                        s->violation = true;
                        s->killed = true;
                        child->terminate();
                    }
                }
            } else {
                if (ev.kind == "agent_end")
                    s->saw_end = true;
                if (ev.kind == "tool_result") {
                    log_.info("agent", "tool_result",
                              {{"session", s->id},
                               {"tool", ev.tool.substr(0, 64)},
                               {"ok", ev.data.value("ok", false) ? "true" : "false"},
                               {"bytes", std::to_string(ev.data.value("bytes", std::size_t{0}))}});
                }
                push(*s, ev.kind, ev.data);
            }
        }
    };
    while (true) {
        const std::size_t n = child->read_stdout(buf, sizeof(buf));
        if (n == 0)
            break;
        for (std::size_t i = 0; i < n; ++i) {
            const char ch = buf[i];
            if (ch == '\n') {
                if (!discarding) {
                    if (!pending.empty() && pending.back() == '\r')
                        pending.pop_back();
                    handle_line(pending);
                }
                pending.clear();
                discarding = false;
            } else if (!discarding) {
                pending.push_back(ch);
                if (pending.size() > lim.max_line_bytes) {
                    pending.clear();
                    discarding = true;
                    push(*s, "log", {{"stream", "stdout"}, {"text", "an oversized output line was dropped"}});
                }
            }
        }
    }
    if (!pending.empty() && !discarding)
        handle_line(pending);
    err_thread.join();

    std::optional<int> code =
        child->wait(s->killed || s->cancel_req || s->timed_out ? lim.kill_grace_ms : 5000);
    if (!code) {
        child->terminate();
        code = child->wait(lim.kill_grace_ms);
    }
    child->terminate();  // reap grandchildren that outlived the main process
    SessionState st;
    std::string reason;
    if (s->violation) {
        st = SessionState::PolicyKilled;
        reason = "terminated after a tool call broke the policy";
    } else if (s->cancel_req) {
        st = SessionState::Cancelled;
        reason = "cancelled by request";
    } else if (s->timed_out) {
        st = SessionState::TimedOut;
        reason = "wall-clock timeout";
    } else if (code && *code == 0 && s->saw_end) {
        st = SessionState::Completed;
        reason = "agent finished";
    } else {
        st = SessionState::Failed;
        reason = code ? "agent exited with code " + std::to_string(*code) : "agent did not exit";
    }
    log_.info("agent", "session ended", {{"session", s->id}, {"state", std::string(to_string(st))}});
    finish(*s, st, code, reason);
    std::lock_guard lk(s->m);
    s->child.reset();
}

void SessionManager::watchdog() {
    std::unique_lock lk(mu_);
    while (!stopping_) {
        wd_cv_.wait_for(lk, std::chrono::milliseconds(100));
        std::vector<std::shared_ptr<Session>> all;
        for (auto& [id, s] : sessions_) all.push_back(s);
        lk.unlock();
        for (auto& s : all) {
            std::shared_ptr<process::IChildProcess> c;
            {
                std::lock_guard sl(s->m);
                if (is_terminal(s->info.state))
                    continue;
                c = s->child;
            }
            if (!s->timed_out && Clock::now() > s->deadline)
                s->timed_out = true;
            if (c && (s->cancel_req || s->timed_out) && !s->killed.exchange(true))
                c->terminate();
        }
        lk.lock();
    }
}

std::vector<SessionInfo> SessionManager::list() const {
    std::vector<std::shared_ptr<Session>> all;
    {
        std::lock_guard lk(mu_);
        for (const auto& id : order_) all.push_back(sessions_.at(id));
    }
    std::vector<SessionInfo> out;
    for (auto& s : all) {
        std::lock_guard lk(s->m);
        out.push_back(s->info);
    }
    return out;
}

Result<SessionInfo> SessionManager::get(const std::string& id) const {
    auto s = valid_id(id) ? find(id) : nullptr;
    if (!s)
        return make_error(ErrorCode::NotFound, "no such agent session");
    std::lock_guard lk(s->m);
    return s->info;
}

Result<EventPage> SessionManager::events(const std::string& id, std::uint64_t after, std::uint32_t wait_ms,
                                         std::size_t max_count) const {
    auto s = valid_id(id) ? find(id) : nullptr;
    if (!s)
        return make_error(ErrorCode::NotFound, "no such agent session");
    std::unique_lock lk(s->m);
    if (wait_ms > 0) {
        s->cv.wait_for(lk, std::chrono::milliseconds(std::min<std::uint32_t>(wait_ms, 60000)),
                       [&] { return s->events.size() > after || is_terminal(s->info.state); });
    }
    EventPage page;
    page.state = s->info.state;
    const std::size_t start = static_cast<std::size_t>(std::min<std::uint64_t>(after, s->events.size()));
    const std::size_t end = std::min(s->events.size(), start + std::max<std::size_t>(1, max_count));
    for (std::size_t i = start; i < end; ++i) page.events.push_back(s->events[i]);
    page.next_seq = end;
    page.finished = is_terminal(s->info.state) && end == s->events.size();
    return page;
}

Result<void> SessionManager::cancel(const std::string& id) {
    auto s = valid_id(id) ? find(id) : nullptr;
    if (!s)
        return make_error(ErrorCode::NotFound, "no such agent session");
    std::shared_ptr<process::IChildProcess> c;
    {
        std::lock_guard lk(s->m);
        if (is_terminal(s->info.state))
            return make_error(ErrorCode::InvalidState, "session already finished");
        c = s->child;
    }
    s->cancel_req = true;
    if (c && !s->killed.exchange(true))
        c->terminate();
    return {};
}

void SessionManager::shutdown() {
    std::vector<std::shared_ptr<Session>> all;
    {
        std::lock_guard lk(mu_);
        stopping_ = true;
        for (auto& [id, s] : sessions_) all.push_back(s);
    }
    wd_cv_.notify_all();
    for (auto& s : all) {
        s->cancel_req = true;
        std::shared_ptr<process::IChildProcess> c;
        {
            std::lock_guard lk(s->m);
            c = s->child;
        }
        if (c)
            c->terminate();
    }
    for (auto& s : all)
        if (s->runner.joinable())
            s->runner.join();
    if (watchdog_.joinable())
        watchdog_.join();
}

}  // namespace hcs::agent
