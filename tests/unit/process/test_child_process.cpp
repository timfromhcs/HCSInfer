#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "hcs/process/child_process.hpp"
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

// HCS_CHILD_PROBE is the build's runtime output directory (set by the CMakeLists); the probe lives there.
fs::path probe() {
    return fs::path(std::string(HCS_CHILD_PROBE) + "/hcs_child_probe" + kExeSuffix);
}

SpawnOptions opts(const fs::path& cwd, std::vector<std::string> args) {
    SpawnOptions o;
    o.cwd = cwd;
    o.args = std::move(args);
    return o;
}

void nap(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

#ifdef _WIN32

TEST(ChildProcess, CapturesMergedStdoutAndStderr) {
    TempDir dir;
    auto started = ChildProcess::spawn(probe(), opts(dir.path(), {"echo", "hello", "world"}));
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();
    auto waited = child.wait_for(10000);
    ASSERT_TRUE(waited) << waited.error().message;
    EXPECT_EQ(waited.value().state, WaitState::Exited);
    EXPECT_EQ(waited.value().exit_code, 0);
    EXPECT_TRUE(contains(child.output(), "hello world"));
    EXPECT_TRUE(contains(child.output(), "to-stderr"));
    EXPECT_FALSE(child.output_truncated());
    EXPECT_GT(child.pid(), 0u);
    EXPECT_FALSE(child.running());
}

TEST(ChildProcess, ReportsTheExitCode) {
    TempDir dir;
    auto started = ChildProcess::spawn(probe(), opts(dir.path(), {"exit", "7"}));
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();
    auto waited = child.wait_for(10000);
    ASSERT_TRUE(waited);
    EXPECT_EQ(waited.value().exit_code, 7);
}

TEST(ChildProcess, TimesOutThenTerminateKills) {
    TempDir dir;
    auto started = ChildProcess::spawn(probe(), opts(dir.path(), {"sleep", "5000"}));
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();

    auto early = child.wait_for(200);
    ASSERT_TRUE(early);
    EXPECT_EQ(early.value().state, WaitState::TimedOut);
    EXPECT_TRUE(child.running());

    ASSERT_TRUE(child.terminate());
    EXPECT_FALSE(child.running());
    // Reaping after a kill reports Exited, not TimedOut.
    auto after = child.wait_for(2000);
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().state, WaitState::Exited);
}

TEST(ChildProcess, CapsCapturedOutput) {
    TempDir dir;
    SpawnOptions o = opts(dir.path(), {"spam", "64"});  // 64 KiB of output
    o.max_output_bytes = 1024;
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();
    auto waited = child.wait_for(10000);
    ASSERT_TRUE(waited);
    EXPECT_EQ(waited.value().state, WaitState::Exited);
    EXPECT_LE(child.output().size(), 1024u);
    EXPECT_TRUE(child.output_truncated());
}

TEST(ChildProcess, DestroyingTheObjectKillsTheWholeTree) {
    TempDir dir;
    const fs::path marker = dir / "grandchild-marker.txt";
    // The grandchild sleeps 800 ms and would write the marker; the tree is killed well before that.
    {
        auto started = ChildProcess::spawn(probe(), opts(dir.path(), {"tree", "800", marker.string()}));
        ASSERT_TRUE(started) << started.error().message;
        ChildProcess child = std::move(started).value();
        auto early = child.wait_for(400);  // the parent sleeps ms + 2000, so it is still running
        ASSERT_TRUE(early);
        EXPECT_EQ(early.value().state, WaitState::TimedOut);
        // Destroying `child` at the end of this scope kills the tree via the job object.
    }
    nap(1600);  // give the (killed) grandchild time to have written the marker if it had survived
    EXPECT_FALSE(fs::exists(marker)) << "the grandchild survived the job-object kill";
}

TEST(ChildProcess, PassesEnvironmentOverrides) {
    TempDir dir;
    SpawnOptions o = opts(dir.path(), {"env", "HCS_TEST_CHILD_ENV"});
    o.env = {{"HCS_TEST_CHILD_ENV", "value-42"}};
    auto started = ChildProcess::spawn(probe(), o);
    ASSERT_TRUE(started) << started.error().message;
    ChildProcess child = std::move(started).value();
    auto waited = child.wait_for(10000);
    ASSERT_TRUE(waited);
    EXPECT_EQ(waited.value().state, WaitState::Exited);
    EXPECT_TRUE(contains(child.output(), "value-42"));
}

TEST(ChildProcess, RejectsARelativeProgram) {
    TempDir dir;
    auto started = ChildProcess::spawn(fs::path("child_probe"), opts(dir.path(), {"echo", "x"}));
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, ErrorCode::InvalidArgument);
}

TEST(ChildProcess, RejectsARelativeCwd) {
    auto started = ChildProcess::spawn(probe(), opts(fs::path("relative"), {"echo", "x"}));
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, ErrorCode::InvalidArgument);
}

#else

TEST(ChildProcess, UnsupportedOnThisPlatform) {
    auto started = ChildProcess::spawn(fs::path("/bin/true"), opts(fs::path("/"), {}));
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, ErrorCode::Unsupported);
}

#endif
