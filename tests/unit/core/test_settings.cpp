#include <gtest/gtest.h>

#include <filesystem>
#include <nlohmann/json.hpp>

#include "hcs/settings.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using hcs::test::TempDir;
namespace fs = std::filesystem;

TEST(Settings, DefaultsAreSafeAndValid) {
    Settings s;
    EXPECT_TRUE(validate(s));
    EXPECT_EQ(s.bind_host, "127.0.0.1");
    EXPECT_FALSE(s.allow_lan);
    EXPECT_EQ(s.schema_version, kSettingsSchemaVersion);
}

TEST(Settings, RoundTripThroughFile) {
    TempDir t;
    Settings s;
    s.port = 9000;
    s.accel = AccelProfile::CpuOnly;
    s.memory = MemoryProfile::LowMemory;
    s.model_roots = {"E:\\models", "D:\\more"};
    s.log_level = "debug";
    s.context_tokens = 8192;
    const auto path = t / "cfg" / "settings.json";
    ASSERT_TRUE(save_settings(path, s));
    auto back = load_settings(path);
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(back.value().port, 9000);
    EXPECT_EQ(back.value().accel, AccelProfile::CpuOnly);
    EXPECT_EQ(back.value().memory, MemoryProfile::LowMemory);
    EXPECT_EQ(back.value().model_roots, s.model_roots);
    EXPECT_EQ(back.value().log_level, "debug");
    EXPECT_EQ(back.value().context_tokens, 8192u);
    EXPECT_EQ(settings_to_json(back.value()), settings_to_json(s));
}

TEST(Settings, MissingFileYieldsDefaults) {
    TempDir t;
    auto s = load_settings(t / "nope.json");
    ASSERT_TRUE(s);
    EXPECT_EQ(s.value().port, 8765);
}

TEST(Settings, AtomicWriteReplacesAndLeavesNoTempFiles) {
    TempDir t;
    const auto path = t / "settings.json";
    Settings a, b;
    a.port = 1111;
    b.port = 2222;
    ASSERT_TRUE(save_settings(path, a));
    ASSERT_TRUE(save_settings(path, b));
    EXPECT_EQ(load_settings(path).value().port, 2222);
    int files = 0;
    for (auto& e : fs::directory_iterator(t.path())) {
        ++files;
        EXPECT_EQ(e.path().filename(), "settings.json") << "stray temp file left behind";
    }
    EXPECT_EQ(files, 1);
}

TEST(Settings, FailedSaveKeepsPreviousFile) {
    TempDir t;
    const auto path = t / "settings.json";
    Settings good;
    good.port = 4242;
    ASSERT_TRUE(save_settings(path, good));
    Settings bad;
    bad.port = 70000;
    auto r = save_settings(path, bad);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(load_settings(path).value().port, 4242);
}

TEST(Settings, MigratesLegacyV0) {
    const char* v0 = R"({"host":"127.0.0.1","port":9001,"model_dir":"E:\\models"})";
    auto s = parse_settings(v0);
    ASSERT_TRUE(s) << s.error().message;
    EXPECT_EQ(s.value().schema_version, 1);
    EXPECT_EQ(s.value().port, 9001);
    ASSERT_EQ(s.value().model_roots.size(), 1u);
    EXPECT_EQ(s.value().model_roots[0], "E:\\models");
    EXPECT_EQ(s.value().bind_host, "127.0.0.1");
}

TEST(Settings, MigratedFileIsRewrittenAtCurrentVersion) {
    TempDir t;
    const auto path = t / "s.json";
    hcs::test::write_file(path, R"({"port":9002})");
    auto s = load_settings(path);
    ASSERT_TRUE(s);
    ASSERT_TRUE(save_settings(path, s.value()));
    auto j = nlohmann::json::parse(hcs::test::read_file(path));
    EXPECT_EQ(j["schema_version"], 1);
}

TEST(Settings, NewerSchemaIsRejectedNotDowngraded) {
    auto s = parse_settings(R"({"schema_version":2,"port":1})");
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, ErrorCode::Unsupported);
}

TEST(Settings, RejectsCorruptAndMistypedInput) {
    EXPECT_EQ(parse_settings("{not json").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_settings("[]").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_settings(R"({"schema_version":"1"})").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_settings(R"({"schema_version":1,"port":"80"})").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_settings(R"({"schema_version":1,"accel":"quantum"})").error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(parse_settings(R"({"schema_version":-1})").error().code, ErrorCode::InvalidArgument);
}

TEST(Settings, IgnoresUnknownKeys) {
    auto s = parse_settings(R"({"schema_version":1,"from_the_future":true})");
    ASSERT_TRUE(s);
}

TEST(Settings, ValidationBounds) {
    Settings s;
    s.port = 0;
    EXPECT_FALSE(validate(s));
    s.port = 65536;
    EXPECT_FALSE(validate(s));
    s.port = 65535;
    EXPECT_TRUE(validate(s));
    s.context_tokens = 10;
    EXPECT_FALSE(validate(s));
    s.context_tokens = 4096;
    s.log_level = "loud";
    EXPECT_FALSE(validate(s));
    s.log_level = "warn";
    s.model_roots = {""};
    EXPECT_FALSE(validate(s));
}

TEST(Settings, NonLoopbackBindNeedsExplicitLanOptIn) {
    Settings s;
    s.bind_host = "0.0.0.0";
    auto r = validate(s);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation);
    s.allow_lan = true;
    EXPECT_TRUE(validate(s));
    for (const char* h : {"127.0.0.1", "localhost", "::1", "[::1]", "127.1.2.3"})
        EXPECT_TRUE(is_loopback_host(h)) << h;
    for (const char* h : {"0.0.0.0", "192.168.1.5", "::", "example.com"})
        EXPECT_FALSE(is_loopback_host(h)) << h;
}

// M10 audit D7: "127." is a prefix of many DNS names; only real 127.0.0.0/8 literals are loopback.
TEST(Settings, LoopbackCheckIsAnIpLiteralParseNotAPrefixMatch) {
    for (const char* h :
         {"127.foo.invalid", "127.example.net", "127.0.0.1.evil.com", "127.", "127.1", "127.1.2",
          "127.0.0.256", "127.0.0.1.1", "127.0.0.1 ", " 127.0.0.1", "127.0.0.01x", "127..0.1", "0127.0.0.1",
          "127.0.0.01", "127.0.0.-1", "::ffff:8.8.8.8", "[::2]", "localhost.evil.com", ""})
        EXPECT_FALSE(is_loopback_host(h)) << h;
    EXPECT_FALSE(is_loopback_host(std::string_view("127.0.0.1\0evil", 14)));
    for (const char* h : {"127.0.0.1", "127.255.255.254", "127.0.0.0", "localhost", "::1", "[::1]"})
        EXPECT_TRUE(is_loopback_host(h)) << h;
    Settings s;
    s.bind_host = "127.example.net";
    ASSERT_FALSE(validate(s));
    EXPECT_EQ(validate(s).error().code, ErrorCode::SecurityViolation);
}

// M10 audit D4 (part): the settings cap must equal the engine cap (1048576), not twice that.
TEST(Settings, ContextTokenCapMatchesEngineCap) {
    Settings s;
    s.context_tokens = 1u << 20;
    EXPECT_TRUE(validate(s));
    s.context_tokens = (1u << 20) + 1;
    EXPECT_FALSE(validate(s));
    s.context_tokens = 1u << 21;
    EXPECT_FALSE(validate(s));
}

TEST(Settings, ProfileNames) {
    EXPECT_EQ(to_string(AccelProfile::CpuOnly), "cpu");
    EXPECT_EQ(to_string(MemoryProfile::LowMemory), "low_memory");
}
