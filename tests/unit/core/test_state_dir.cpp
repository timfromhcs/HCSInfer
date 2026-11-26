// HCS_DATA_DIR must redirect every HCS-owned state location and never fall back to the per-user profile
// (%LOCALAPPDATA% / %APPDATA%, normally on C:) when it is set ("nothing on C:" guard).
#include <gtest/gtest.h>

#include <map>

#include "hcs/state_dir.hpp"

using namespace hcs;

namespace {
EnvLookup fake_env(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const char* name) {
        auto it = vars.find(name);
        return it == vars.end() ? std::string() : it->second;
    };
}
}  // namespace

TEST(StateDir, DataDirWinsOverUserProfile) {
    auto env = fake_env({{"HCS_DATA_DIR", "D:\\HCSData"},
                         {"LOCALAPPDATA", "C:\\Users\\x\\AppData\\Local"},
                         {"HOME", "C:\\Users\\x"}});
    EXPECT_EQ(resolve_state_dir("", env), std::filesystem::path("D:\\HCSData"));
}

TEST(StateDir, DataDirNeverFallsBackToLocalAppData) {
    auto env = fake_env({{"HCS_DATA_DIR", "D:\\HCSData"}, {"LOCALAPPDATA", "C:\\Users\\x\\AppData\\Local"}});
    const auto base = resolve_state_dir("", env);
    EXPECT_EQ(base.string().find("AppData"), std::string::npos);
    EXPECT_EQ(base.string().find("C:"), std::string::npos);
    // Every file the app derives from the state dir stays below it.
    for (const char* leaf :
         {"settings.json", "registry.json", "sessions", "train", "home", "logs", "cache"}) {
        const auto child = (base / leaf).string();
        EXPECT_EQ(child.rfind(base.string(), 0), 0u) << leaf;
        EXPECT_EQ(child.find("C:"), std::string::npos) << leaf;
    }
}

TEST(StateDir, ExplicitOverrideBeatsDataDir) {
    auto env = fake_env({{"HCS_DATA_DIR", "D:\\HCSData"}});
    EXPECT_EQ(resolve_state_dir("E:\\custom", env), std::filesystem::path("E:\\custom"));
}

TEST(StateDir, DataDirBeatsLegacyStateDir) {
    auto env = fake_env({{"HCS_DATA_DIR", "D:\\New"}, {"HCS_STATE_DIR", "D:\\Old"}});
    EXPECT_EQ(resolve_state_dir("", env), std::filesystem::path("D:\\New"));
}

TEST(StateDir, LegacyDefaultsUnchangedWhenDataDirUnset) {
    EXPECT_EQ(resolve_state_dir("", fake_env({{"HCS_STATE_DIR", "D:\\Old"}})),
              std::filesystem::path("D:\\Old"));
    EXPECT_EQ(resolve_state_dir("", fake_env({{"LOCALAPPDATA", "C:\\L"}})),
              std::filesystem::path("C:\\L") / "HCSInfer");
    EXPECT_EQ(resolve_state_dir("", fake_env({{"HOME", "/h"}})), std::filesystem::path("/h") / ".hcsinfer");
    EXPECT_EQ(resolve_state_dir("", fake_env({})), std::filesystem::path(".hcsinfer"));
}
