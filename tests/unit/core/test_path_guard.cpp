#include <gtest/gtest.h>

#include <cwctype>
#include <filesystem>

#include "hcs/path_guard.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using hcs::test::TempDir;
namespace fs = std::filesystem;

namespace {
struct Fixture {
    TempDir tmp;
    fs::path root = tmp / "root";
    fs::path outside = tmp / "outside";
    PathGuard guard;
    Fixture() {
        fs::create_directories(root / "models");
        fs::create_directories(outside);
        hcs::test::write_file(root / "models" / "a.gguf", "x");
        hcs::test::write_file(outside / "secret.txt", "s");
        EXPECT_TRUE(guard.add_root(root));
    }
};
}  // namespace

TEST(PathGuard, AcceptsPathsInsideRoot) {
    Fixture f;
    auto r = f.guard.resolve(f.root / "models" / "a.gguf");
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(fs::equivalent(r.value(), f.root / "models" / "a.gguf"));
    EXPECT_TRUE(f.guard.resolve(f.root));                          // the root itself
    EXPECT_TRUE(f.guard.resolve(f.root / "models" / "new.gguf"));  // not yet existing (weak canonical)
}

TEST(PathGuard, RejectsDotDotTraversal) {
    Fixture f;
    auto r = f.guard.resolve(f.root / "models" / ".." / ".." / "outside" / "secret.txt");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation);
    // Even a ".." that would stay inside is rejected: no lexical tricks accepted.
    EXPECT_EQ(f.guard.resolve(f.root / "models" / ".." / "models").error().code, ErrorCode::SecurityViolation);
}

TEST(PathGuard, RejectsAbsolutePathOutside) {
    Fixture f;
    auto r = f.guard.resolve(f.outside / "secret.txt");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation);
}

TEST(PathGuard, RejectsSiblingWithSharedPrefix) {
    Fixture f;
    fs::create_directories(f.tmp / "root-evil");
    auto r = f.guard.resolve(f.tmp / "root-evil" / "x.gguf");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation);
}

TEST(PathGuard, RejectsEmptyAndNul) {
    Fixture f;
    EXPECT_EQ(f.guard.resolve("").error().code, ErrorCode::InvalidArgument);
    std::string with_nul = "a";
    with_nul.push_back('\0');
    with_nul += "b";
    EXPECT_FALSE(f.guard.resolve(fs::path(with_nul)));
}

TEST(PathGuard, EmptyGuardRejectsEverything) {
    PathGuard g;
    EXPECT_TRUE(g.empty());
    TempDir t;
    EXPECT_EQ(g.resolve(t.path()).error().code, ErrorCode::SecurityViolation);
}

TEST(PathGuard, AddRootValidation) {
    PathGuard g;
    TempDir t;
    EXPECT_EQ(g.add_root(t / "missing").error().code, ErrorCode::NotFound);
    hcs::test::write_file(t / "file.txt", "x");
    EXPECT_EQ(g.add_root(t / "file.txt").error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(g.add_root(t / "a" / ".." / "b"));
    EXPECT_TRUE(g.add_root(t.path()));
    EXPECT_TRUE(g.add_root(t.path()));  // idempotent
    EXPECT_EQ(g.roots().size(), 1u);
}

TEST(PathGuard, LinkInsideRootPointingOutsideIsRejected) {
    Fixture f;
    const fs::path link = f.root / "link";
    if (!hcs::test::make_dir_link(link, f.outside)) GTEST_SKIP() << "cannot create symlink/junction here";
    auto r = f.guard.resolve(link / "secret.txt");
    ASSERT_FALSE(r) << "escape through link was accepted: " << r.value().string();
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation);
    EXPECT_FALSE(f.guard.resolve(link));
}

TEST(PathGuard, LinkInsideRootPointingInsideIsAccepted) {
    Fixture f;
    const fs::path link = f.root / "alias";
    if (!hcs::test::make_dir_link(link, f.root / "models")) GTEST_SKIP() << "cannot create symlink/junction here";
    auto r = f.guard.resolve(link / "a.gguf");
    ASSERT_TRUE(r) << r.error().message;
    // Resolved form points at the real location, inside the root.
    EXPECT_TRUE(fs::equivalent(r.value(), f.root / "models" / "a.gguf"));
}

#ifdef _WIN32
TEST(PathGuard, WindowsCaseInsensitiveRootMatch) {
    Fixture f;
    std::wstring s = (f.root / "models" / "a.gguf").wstring();
    for (auto& c : s) c = static_cast<wchar_t>(std::towupper(c));
    EXPECT_TRUE(f.guard.resolve(fs::path(s)));
}

TEST(PathGuard, WindowsRejectsDeviceNamespaceAndStreams) {
    Fixture f;
    EXPECT_EQ(f.guard.resolve(fs::path(L"\\\\?\\C:\\Windows\\win.ini")).error().code, ErrorCode::SecurityViolation);
    EXPECT_EQ(f.guard.resolve(fs::path(L"\\\\.\\PhysicalDrive0")).error().code, ErrorCode::SecurityViolation);
    auto stream = (f.root / "models" / "a.gguf").wstring() + L":hidden";
    EXPECT_EQ(f.guard.resolve(fs::path(stream)).error().code, ErrorCode::SecurityViolation);
}
#endif

#ifdef _WIN32
// M10 audit D5: Win32 maps <dir>\NUL, <dir>\CON.txt, <dir>\COM1 ... to DOS devices even inside an approved
// root, and silently strips trailing dots/spaces. The guard must reject them as SecurityViolation.
TEST(PathGuard, WindowsRejectsDosDeviceNamesInsideRoot) {
    Fixture f;
    const std::vector<std::wstring> bad = {
        L"CON",       L"con",         L"NUL",        L"NUL.png",     L"nul.tar.gz",  L"COM1",
        L"com9.txt",  L"LPT1",        L"lpt9.bin",   L"PRN",         L"AUX",         L"CONIN$",
        L"CONOUT$",   L"CLOCK$",      L"nul .txt",   L"COM1  ",      L"con.",        L"aux ...",
        L"COM¹", L"LPT².x", L"COM³"};
    for (const auto& name : bad) {
        auto r = f.guard.resolve((f.root / "models" / name));
        ASSERT_FALSE(r) << "accepted reserved name";
        EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation) << r.error().message;
    }
    // Reserved name as an intermediate directory component is just as dangerous.
    EXPECT_EQ(f.guard.resolve(f.root / "NUL" / "x.gguf").error().code, ErrorCode::SecurityViolation);
}

TEST(PathGuard, WindowsRejectsWildcardsAndTrailingDotsInsideRoot) {
    Fixture f;
    const std::vector<std::wstring> bad = {L"*.png", L"a?.png", L"a<b", L"a>b", L"a|b", L"a\"b",
                                           L"model.gguf.", L"model.gguf ", L"model.gguf. ", L"ctl\x01x"};
    for (const auto& name : bad) {
        auto r = f.guard.resolve(f.root / "models" / name);
        ASSERT_FALSE(r) << "accepted unsafe name";
        EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation) << r.error().message;
    }
    EXPECT_EQ(f.guard.resolve(f.root / "models" / std::wstring(300, L'a')).error().code, ErrorCode::SecurityViolation);
}

TEST(PathGuard, WindowsStillAcceptsLookalikeLegitimateNames) {
    Fixture f;
    for (const wchar_t* name : {L"console.gguf", L"comet.png", L"nullable.bin", L"COM10.txt", L"LPT0", L"a.b.gguf",
                                L"con-sole", L"auxiliary", L"prn_2", L".hidden", L"model (1).gguf"}) {
        auto r = f.guard.resolve(f.root / "models" / name);
        EXPECT_TRUE(r) << "rejected a legitimate name: " << r.error().message;
    }
}
#endif
