// Streaming reads, replaced environment, Job Object caps and the IProcessLauncher seam, against the real
// helper executable (hcs_child_probe). Windows only; elsewhere the launcher reports Unsupported.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "hcs/process/child_process.hpp"
#include "hcs/process/launcher.hpp"
#include "hcs/result.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::process;
using hcs::test::TempDir;

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

fs::path probe() {
    return fs::path(std::string(HCS_CHILD_PROBE) + "/hcs_child_probe" + kExeSuffix);
}

std::string getenv_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
}

void nap(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

template <typename ReadFn>
std::string drain(ReadFn&& read) {
    std::string out;
    char buf[256];
    for (;;) {
        const std::size_t n = read(buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        out.append(buf, n);
    }
    return out;
}

// The minimal environment a console helper needs to start when nothing is inherited.
std::map<std::string, std::string> minimal_env() {
    std::map<std::string, std::string> env;
    for (const char* name : {"SystemRoot", "windir", "PATH"}) {
        const std::string v = getenv_or_empty(name);
        if (!v.empty()) {
            env[name] = v;
        }
    }
    return env;
}

int count_markers(const fs::path& dir, const std::string& prefix, int n) {
    int found = 0;
    for (int i = 0; i < n; ++i) {
        found += fs::exists(dir / (prefix + std::to_string(i))) ? 1 : 0;
    }
    return found;
}

}  // namespace

#ifdef _WIN32

// ---------------------------------------------------------------------------------------------------------------
// ChildProcess: streaming mode
// ---------------------------------------------------------------------------------------------------------------
TEST(ChildProcessStream, StdoutAndStderrArriveOnSeparatePipes) {
    TempDir dir;
    SpawnOptions o;
    o.cwd = dir.path();
    o.args = {"streams"};
    o.stream_output = true;
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();
    std::string err;
    std::thread err_reader(
        [&] { err = drain([&](char* b, std::size_t c) { return child.read_stderr(b, c); }); });
    const std::string out = drain([&](char* b, std::size_t c) { return child.read_stdout(b, c); });
    err_reader.join();
    EXPECT_EQ(out, "out1\r\nout2\r\n");
    EXPECT_EQ(err, "err1\r\n");
    auto waited = child.wait_for(10000);
    ASSERT_TRUE(waited);
    EXPECT_EQ(waited.value().exit_code, 0);
    EXPECT_TRUE(child.output().empty()) << "streaming mode must not buffer internally";
}

TEST(ChildProcessStream, ReadsReturnZeroWhenNotStreaming) {
    TempDir dir;
    SpawnOptions o;
    o.cwd = dir.path();
    o.args = {"echo", "x"};
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started);
    ChildProcess child = std::move(started).value();
    char buf[16];
    EXPECT_EQ(child.read_stdout(buf, sizeof(buf)), 0u);
    EXPECT_EQ(child.read_stderr(buf, sizeof(buf)), 0u);
    ASSERT_TRUE(child.wait_for(10000));
    ChildProcess empty;
    EXPECT_EQ(empty.read_stdout(buf, sizeof(buf)), 0u);
}

TEST(ChildProcessStream, TerminateFromAnotherThreadUnblocksBlockedReaders) {
    TempDir dir;
    SpawnOptions o;
    o.cwd = dir.path();
    o.args = {"sleep", "30000"};
    o.stream_output = true;
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();
    std::atomic<int> returned{0};
    std::thread r1([&] {
        char b[16];
        (void)child.read_stdout(b, sizeof(b));
        ++returned;
    });
    std::thread r2([&] {
        char b[16];
        (void)child.read_stderr(b, sizeof(b));
        ++returned;
    });
    nap(300);
    EXPECT_EQ(returned.load(), 0) << "readers should still be blocked while the child runs";
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(child.terminate());
    r1.join();
    r2.join();
    EXPECT_EQ(returned.load(), 2);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
}

TEST(ChildProcessStream, TerminateRacingWithWaitAndRepeatedTerminateAreSafe) {
    TempDir dir;
    SpawnOptions o;
    o.cwd = dir.path();
    o.args = {"sleep", "30000"};
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started);
    ChildProcess child = std::move(started).value();
    std::atomic<bool> exited{false};
    std::thread waiter([&] {
        auto w = child.wait_for(20000);
        exited = w && w.value().state == WaitState::Exited;
    });
    nap(200);
    std::thread t1([&] { (void)child.terminate(); });
    std::thread t2([&] { (void)child.terminate(); });
    t1.join();
    t2.join();
    waiter.join();
    EXPECT_TRUE(exited.load());
    EXPECT_TRUE(child.terminate());  // idempotent
    auto w = child.wait_for(0);
    ASSERT_TRUE(w);
    EXPECT_EQ(w.value().state, WaitState::Exited);
    EXPECT_NE(w.value().exit_code, 0) << "a killed child reports a non-zero exit code";
}

// ---------------------------------------------------------------------------------------------------------------
// ChildProcess: environment and Job Object caps
// ---------------------------------------------------------------------------------------------------------------
TEST(ChildProcessEnv, DefaultInheritsTheParentEnvironment) {
    ASSERT_EQ(_putenv_s("HCS_TEST_PARENT_ONLY", "parent-value"), 0);
    TempDir dir;
    SpawnOptions o;
    o.cwd = dir.path();
    o.args = {"env", "HCS_TEST_PARENT_ONLY"};
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started);
    ChildProcess child = std::move(started).value();
    ASSERT_TRUE(child.wait_for(10000));
    EXPECT_TRUE(contains(child.output(), "parent-value"));
}

TEST(ChildProcessEnv, ReplacedEnvironmentContainsOnlyWhatWasGiven) {
    ASSERT_EQ(_putenv_s("HCS_TEST_PARENT_SECRET", "must-not-leak"), 0);
    TempDir dir;
    SpawnOptions o;
    o.cwd = dir.path();
    o.args = {"envnames"};
    o.inherit_environment = false;
    o.env = minimal_env();
    o.env["ONLY_ME"] = "1";
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();
    auto waited = child.wait_for(10000);
    ASSERT_TRUE(waited);
    ASSERT_EQ(waited.value().exit_code, 0) << child.output();
    const std::string names = child.output();
    EXPECT_TRUE(contains(names, "ONLY_ME"));
    EXPECT_FALSE(contains(names, "HCS_TEST_PARENT_SECRET"));
    EXPECT_FALSE(contains(names, "USERPROFILE"));
    EXPECT_FALSE(contains(names, "APPDATA"));
}

TEST(ChildProcessEnv, EmptyReplacedEnvironmentIsValid) {
    TempDir dir;
    SpawnOptions o;
    o.cwd = dir.path();
    o.args = {"env", "SystemRoot"};
    o.inherit_environment = false;
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();
    ASSERT_TRUE(child.wait_for(10000));
    EXPECT_TRUE(contains(child.output(), "(unset)")) << child.output();
}

TEST(ChildProcessJob, ActiveProcessLimitStopsAForkBomb) {
    TempDir dir;
    const int n = 6;
    {
        SpawnOptions o;
        o.cwd = dir.path();
        o.args = {"fork", std::to_string(n), "300", (dir / "free-").string()};
        auto started = ChildProcess::spawn(probe(), o);
        ASSERT_TRUE(started);
        ChildProcess child = std::move(started).value();
        ASSERT_TRUE(child.wait_for(15000));
        EXPECT_EQ(count_markers(dir.path(), "free-", n), n) << "control: without a cap every child runs";
    }
    {
        SpawnOptions o;
        o.cwd = dir.path();
        o.args = {"fork", std::to_string(n), "300", (dir / "capped-").string()};
        o.max_active_processes = 3;  // the probe itself + 2 children
        auto started = ChildProcess::spawn(probe(), o);
        ASSERT_TRUE(started) << started.error().message;
        ChildProcess child = std::move(started).value();
        ASSERT_TRUE(child.wait_for(15000));
        const int alive = count_markers(dir.path(), "capped-", n);
        EXPECT_LE(alive, 2) << "the Job Object must refuse processes beyond the cap";
        EXPECT_LT(alive, n);
    }
}

TEST(ChildProcessJob, MemoryLimitStopsARunawayAllocation) {
    TempDir dir;
    {
        SpawnOptions o;
        o.cwd = dir.path();
        o.args = {"alloc", "16"};
        o.max_job_memory_bytes = std::uint64_t{512} * 1024 * 1024;
        auto started = ChildProcess::spawn(probe(), o);
        ASSERT_TRUE(started) << started.error().message;
        ChildProcess child = std::move(started).value();
        nap(800);
        EXPECT_TRUE(contains(child.output(), "alloc-ok")) << "a small allocation under the cap must work";
        (void)child.terminate();
    }
    {
        SpawnOptions o;
        o.cwd = dir.path();
        o.args = {"alloc", "400"};
        o.max_job_memory_bytes = std::uint64_t{128} * 1024 * 1024;
        auto started = ChildProcess::spawn(probe(), o);
        ASSERT_TRUE(started) << started.error().message;
        ChildProcess child = std::move(started).value();
        auto waited = child.wait_for(15000);
        ASSERT_TRUE(waited);
        EXPECT_FALSE(contains(child.output(), "alloc-ok"))
            << "the cap must prevent a 400 MiB commit under 128 MiB";
        EXPECT_NE(waited.value().exit_code, 0);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// IProcessLauncher (native)
// ---------------------------------------------------------------------------------------------------------------
namespace {
LaunchSpec spec_for(const TempDir& dir, std::vector<std::string> args) {
    LaunchSpec s;
    s.exe = probe();
    s.cwd = dir.path();
    s.args = std::move(args);
    s.env = minimal_env();
    s.active_process_limit = 8;
    return s;
}
}  // namespace

TEST(NativeLauncher, StreamsStdoutAndStderrSeparatelyAndReportsExitCode) {
    TempDir dir;
    auto launcher = make_native_launcher();
    auto launched = launcher->launch(spec_for(dir, {"streams"}));
    ASSERT_TRUE(launched) << launched.error().message;
    auto child = std::move(launched).value();
    EXPECT_GT(child->pid(), 0u);
    std::string err;
    std::thread t([&] { err = drain([&](char* b, std::size_t c) { return child->read_stderr(b, c); }); });
    const std::string out = drain([&](char* b, std::size_t c) { return child->read_stdout(b, c); });
    t.join();
    EXPECT_EQ(out, "out1\r\nout2\r\n");
    EXPECT_EQ(err, "err1\r\n");
    const auto code = child->wait(10000);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 0);
}

TEST(NativeLauncher, ChildSeesOnlyTheSuppliedEnvironment) {
    ASSERT_EQ(_putenv_s("HCS_TEST_PARENT_SECRET2", "must-not-leak"), 0);
    TempDir dir;
    LaunchSpec spec = spec_for(dir, {"envnames"});
    spec.env["HCS_AGENT_TOKEN"] = "tok-0123456789abcdef";
    auto launched = make_native_launcher()->launch(spec);
    ASSERT_TRUE(launched) << launched.error().message;
    auto child = std::move(launched).value();
    const std::string names = drain([&](char* b, std::size_t c) { return child->read_stdout(b, c); });
    EXPECT_TRUE(contains(names, "HCS_AGENT_TOKEN"));
    EXPECT_FALSE(contains(names, "HCS_TEST_PARENT_SECRET2"));
    EXPECT_FALSE(contains(names, "USERPROFILE"));
    EXPECT_FALSE(contains(names, "TEMP"));
}

TEST(NativeLauncher, WaitReturnsNulloptWhileRunningAndTerminateEndsTheTree) {
    TempDir dir;
    const fs::path marker = dir / "grandchild-marker.txt";
    auto launched = make_native_launcher()->launch(spec_for(dir, {"tree", "800", marker.string()}));
    ASSERT_TRUE(launched) << launched.error().message;
    auto child = std::move(launched).value();
    EXPECT_FALSE(child->wait(300).has_value());
    child->terminate();
    child->terminate();  // idempotent
    EXPECT_TRUE(child->wait(5000).has_value());
    (void)drain(
        [&](char* b, std::size_t c) { return child->read_stdout(b, c); });  // reaches EOF, never blocks
    nap(1600);
    EXPECT_FALSE(fs::exists(marker)) << "the grandchild survived terminate()";
}

TEST(NativeLauncher, DroppingTheChildKillsItsTree) {
    TempDir dir;
    const fs::path marker = dir / "dropped-marker.txt";
    {
        auto launched = make_native_launcher()->launch(spec_for(dir, {"tree", "800", marker.string()}));
        ASSERT_TRUE(launched) << launched.error().message;
        auto child = std::move(launched).value();
        EXPECT_FALSE(child->wait(300).has_value());
    }
    nap(1600);
    EXPECT_FALSE(fs::exists(marker));
}

TEST(NativeLauncher, ActiveProcessAndMemoryCapsAreApplied) {
    TempDir dir;
    LaunchSpec fork = spec_for(dir, {"fork", "6", "300", (dir / "l-").string()});
    fork.active_process_limit = 3;
    auto launched = make_native_launcher()->launch(fork);
    ASSERT_TRUE(launched) << launched.error().message;
    auto child = std::move(launched).value();
    ASSERT_TRUE(child->wait(15000).has_value());
    EXPECT_LE(count_markers(dir.path(), "l-", 6), 2);

    LaunchSpec hog = spec_for(dir, {"alloc", "400"});
    hog.memory_limit_bytes = std::uint64_t{128} * 1024 * 1024;
    auto hog_launched = make_native_launcher()->launch(hog);
    ASSERT_TRUE(hog_launched) << hog_launched.error().message;
    auto hog_child = std::move(hog_launched).value();
    const std::string out = drain([&](char* b, std::size_t c) { return hog_child->read_stdout(b, c); });
    EXPECT_FALSE(contains(out, "alloc-ok"));
}

TEST(NativeLauncher, RejectsRelativePathsAndMissingExecutables) {
    TempDir dir;
    auto launcher = make_native_launcher();
    LaunchSpec relative = spec_for(dir, {"echo"});
    relative.exe = fs::path("hcs_child_probe.exe");
    auto a = launcher->launch(relative);
    ASSERT_FALSE(a);
    EXPECT_EQ(a.error().code, ErrorCode::InvalidArgument);
    LaunchSpec bad_cwd = spec_for(dir, {"echo"});
    bad_cwd.cwd = fs::path("relative-dir");
    auto b = launcher->launch(bad_cwd);
    ASSERT_FALSE(b);
    EXPECT_EQ(b.error().code, ErrorCode::InvalidArgument);
    LaunchSpec missing = spec_for(dir, {"echo"});
    missing.exe = dir / "does-not-exist.exe";
    auto c = launcher->launch(missing);
    EXPECT_FALSE(c);
}

#else

TEST(NativeLauncher, UnsupportedOnThisPlatform) {
    LaunchSpec spec;
    spec.exe = "/bin/true";
    spec.cwd = "/";
    auto r = make_native_launcher()->launch(spec);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
}

#endif
