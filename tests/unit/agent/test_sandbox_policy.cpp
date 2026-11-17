#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hcs/agent/sandbox_policy.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::agent;
using hcs::test::TempDir;
namespace fs = std::filesystem;

namespace {

struct Fixture {
    TempDir tmp;
    fs::path root = tmp / "ws";
    fs::path outside = tmp / "elsewhere";
    ToolSandbox sandbox = make();

    Fixture() = default;

    ToolSandbox make(SandboxPolicy policy = default_policy()) {
        fs::create_directories(root / "src");
        fs::create_directories(root / ".git");
        fs::create_directories(outside);
        PathGuard g;
        EXPECT_TRUE(g.add_root(root));
        auto r = ToolSandbox::create(std::move(g), std::move(policy));
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return std::move(r).value();
    }

    CommandRequest cmd(std::vector<std::string> argv) const {
        CommandRequest c;
        c.argv = std::move(argv);
        c.cwd = root;
        return c;
    }
};

ErrorCode code_of(const Result<ApprovedCommand>& r) {
    return r ? ErrorCode::Ok : r.error().code;
}

}  // namespace

TEST(SandboxPolicy, CreateRejectsHardDeniedAndMalformedRules) {
    TempDir tmp;
    fs::create_directories(tmp / "ws");
    auto guard = [&] {
        PathGuard g;
        EXPECT_TRUE(g.add_root(tmp / "ws"));
        return g;
    };
    for (const char* shell : {"powershell", "cmd", "bash", "python", "curl"}) {
        SandboxPolicy p;
        p.commands = {{shell, {}, {}}};
        auto r = ToolSandbox::create(guard(), p);
        ASSERT_FALSE(r) << shell;
        EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation) << shell;
    }
    {
        SandboxPolicy p;
        p.commands = {{"Git", {}, {}}};  // not canonical
        EXPECT_EQ(ToolSandbox::create(guard(), p).error().code, ErrorCode::InvalidArgument);
    }
    {
        SandboxPolicy p;
        p.commands = {{"git", {}, {}}, {"git", {"status"}, {}}};
        EXPECT_EQ(ToolSandbox::create(guard(), p).error().code, ErrorCode::AlreadyExists);
    }
    {
        SandboxPolicy p = default_policy();
        p.limits.max_output_bytes = 0;
        EXPECT_EQ(ToolSandbox::create(guard(), p).error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_EQ(ToolSandbox::create(PathGuard{}, default_policy()).error().code, ErrorCode::InvalidArgument);
}

TEST(SandboxPolicy, AllowsListedCommandWithDefaultsAndPinsCwd) {
    Fixture f;
    auto r = f.sandbox.authorize_command(f.cmd({"git", "status", "--short"}));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().timeout_ms, default_policy().limits.default_timeout_ms);
    EXPECT_FALSE(r.value().timeout_clamped);
    EXPECT_TRUE(fs::equivalent(r.value().cwd, f.root));
    EXPECT_EQ(r.value().argv, (std::vector<std::string>{"git", "status", "--short"}));
    // A program name with ".exe" matches the same rule.
    EXPECT_TRUE(f.sandbox.authorize_command(f.cmd({"GIT.EXE", "log"})));
    EXPECT_TRUE(f.sandbox.authorize_command(f.cmd({"rg", "TODO", "src"})));
}

TEST(SandboxPolicy, RejectsUnlistedShellsAndPathPrograms) {
    Fixture f;
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"powershell", "-c", "dir"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"cmd.exe", "/c", "dir"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"notepad"}))), ErrorCode::SecurityViolation);
    // Paths, script hosts, trailing-dot/extension tricks and metacharacters are not "bare names".
    for (const char* p : {"C:\\Windows\\System32\\cmd.exe", "./git", "..\\git", "git.cmd", "git.bat", "git.",
                          "git;ls", "git status", "git\"", ""}) {
        EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({p}))), ErrorCode::SecurityViolation) << p;
    }
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({}))), ErrorCode::InvalidArgument);
}

TEST(SandboxPolicy, SubcommandMustBeArgvOneAndAllowListed) {
    Fixture f;
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"git", "push"}))), ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"git"}))), ErrorCode::SecurityViolation);
    // Options before the sub-command are how `git -c core.sshCommand=... fetch` style escapes start.
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"git", "-c", "alias.x=!sh", "status"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"git", "-C", "..", "status"}))),
              ErrorCode::SecurityViolation);
}

TEST(SandboxPolicy, DeniedArgumentsAreRejectedAlsoWithEqualsForm) {
    Fixture f;
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"git", "diff", "--output=x.txt"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"git", "diff", "--EXT-DIFF"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", "--pre=evil"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", "--pre", "evil"}))),
              ErrorCode::SecurityViolation);
    EXPECT_TRUE(f.sandbox.authorize_command(f.cmd({"git", "diff", "--stat"})));
}

TEST(SandboxPolicy, NetworkIsOffByDefault) {
    Fixture f;
    EXPECT_FALSE(default_policy().allow_network);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"cmake", "-DFOO=https://example.com/x"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", "http://example.com"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"cmake", "--fetch=git://host/repo"}))),
              ErrorCode::SecurityViolation);
}

TEST(SandboxPolicy, PathArgumentsMustStayInsideApprovedRoots) {
    Fixture f;
    const std::string inside = (f.root / "src" / "a.cpp").string();
    const std::string outside = (f.outside / "x.txt").string();
    EXPECT_TRUE(f.sandbox.authorize_command(f.cmd({"rg", "x", inside})));
    EXPECT_TRUE(f.sandbox.authorize_command(f.cmd({"rg", "x", "src"})));  // relative, no ".."
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", outside}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", "--file=" + outside}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", "..\\elsewhere"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", "src/../../elsewhere"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", "C:\\Windows\\System32"}))),
              ErrorCode::SecurityViolation);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", "x", "\\\\server\\share\\f"}))),
              ErrorCode::SecurityViolation);
}

TEST(SandboxPolicy, WorkingDirectoryIsGuarded) {
    Fixture f;
    CommandRequest c = f.cmd({"git", "status"});
    c.cwd = f.outside;
    EXPECT_EQ(code_of(f.sandbox.authorize_command(c)), ErrorCode::SecurityViolation);
    c.cwd = f.root / "src" / ".." / "..";
    EXPECT_EQ(code_of(f.sandbox.authorize_command(c)), ErrorCode::SecurityViolation);
    c.cwd = "relative";
    EXPECT_EQ(code_of(f.sandbox.authorize_command(c)), ErrorCode::InvalidArgument);
    c.cwd = f.root / "missing";
    EXPECT_EQ(code_of(f.sandbox.authorize_command(c)), ErrorCode::NotFound);
    c.cwd = f.root / "src";
    EXPECT_TRUE(f.sandbox.authorize_command(c));
}

TEST(SandboxPolicy, TimeoutIsDefaultedClampedAndNeverZero) {
    Fixture f;
    CommandRequest c = f.cmd({"ctest"});
    c.timeout_ms = 5'000;
    auto r = f.sandbox.authorize_command(c);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().timeout_ms, 5'000u);
    EXPECT_FALSE(r.value().timeout_clamped);

    c.timeout_ms = 10'000'000;
    r = f.sandbox.authorize_command(c);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().timeout_ms, default_policy().limits.max_timeout_ms);
    EXPECT_TRUE(r.value().timeout_clamped);

    c.timeout_ms = 0;
    EXPECT_EQ(code_of(f.sandbox.authorize_command(c)), ErrorCode::InvalidArgument);
}

TEST(SandboxPolicy, ArgumentCountAndSizeAndNulAreBounded) {
    Fixture f;
    std::vector<std::string> many{"rg"};
    for (std::size_t i = 0; i < default_policy().limits.max_argc; ++i) many.push_back("x");
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd(many))), ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(
                  f.cmd({"rg", std::string(default_policy().limits.max_arg_bytes + 1, 'a')}))),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(code_of(f.sandbox.authorize_command(f.cmd({"rg", std::string("a\0b", 3)}))),
              ErrorCode::InvalidArgument);
}

TEST(SandboxPolicy, ReadAndWritePaths) {
    Fixture f;
    EXPECT_TRUE(f.sandbox.authorize_read(f.root / "src" / "a.cpp"));
    EXPECT_EQ(f.sandbox.authorize_read(f.outside / "x").error().code, ErrorCode::SecurityViolation);
    EXPECT_EQ(f.sandbox.authorize_read("relative.txt").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(f.sandbox.authorize_read(f.root / ".." / "elsewhere").error().code,
              ErrorCode::SecurityViolation);

    EXPECT_TRUE(f.sandbox.authorize_write(f.root / "src" / "new.cpp", 10));
    EXPECT_EQ(
        f.sandbox.authorize_write(f.root / "src" / "new.cpp", default_policy().limits.max_write_bytes + 1)
            .error()
            .code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(f.sandbox.authorize_write(f.root / "src", 1).error().code,
              ErrorCode::InvalidArgument);  // directory
    EXPECT_EQ(f.sandbox.authorize_write(f.root / ".git" / "hooks" / "pre-commit", 1).error().code,
              ErrorCode::SecurityViolation);
    EXPECT_EQ(f.sandbox.authorize_write(f.root / "GIT~1" / "config", 1).error().code,
              ErrorCode::SecurityViolation);
    EXPECT_EQ(f.sandbox.authorize_write(f.outside / "x", 1).error().code, ErrorCode::SecurityViolation);
}

TEST(SandboxPolicy, OutputCapTruncatesOnUtf8Boundary) {
    TempDir tmp;
    fs::create_directories(tmp / "ws");
    PathGuard g;
    ASSERT_TRUE(g.add_root(tmp / "ws"));
    SandboxPolicy p = default_policy();
    p.limits.max_output_bytes = 5;
    auto sb = ToolSandbox::create(std::move(g), p);
    ASSERT_TRUE(sb);

    auto small = sb.value().cap_output("abc");
    EXPECT_FALSE(small.truncated);
    EXPECT_EQ(small.text, "abc");

    auto exact = sb.value().cap_output("abcde");
    EXPECT_FALSE(exact.truncated);

    auto cut = sb.value().cap_output("abcdefgh");
    EXPECT_TRUE(cut.truncated);
    EXPECT_EQ(cut.text, "abcde");
    EXPECT_EQ(cut.original_bytes, 8u);

    // "ab" + U+00E9 (2 bytes) + U+00E9: the cap at 5 would split the second e-acute; keep 4 bytes.
    const std::string utf8 = "ab\xC3\xA9\xC3\xA9";
    ASSERT_EQ(utf8.size(), 6u);
    auto u = sb.value().cap_output(utf8);
    EXPECT_TRUE(u.truncated);
    EXPECT_EQ(u.text, "ab\xC3\xA9");
}
