#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <sstream>

#include "hcs/logger.hpp"

using namespace hcs;

TEST(Logger, WritesOneJsonObjectPerLine) {
    std::ostringstream os;
    Logger log(&os, LogLevel::Info);
    log.info("core", "hello \"world\"\n", {{"k", "v"}});
    auto j = nlohmann::json::parse(os.str());
    EXPECT_EQ(j["level"], "info");
    EXPECT_EQ(j["component"], "core");
    EXPECT_EQ(j["msg"], "hello \"world\"\n");
    EXPECT_EQ(j["k"], "v");
    EXPECT_TRUE(j.contains("ts"));
    EXPECT_EQ(os.str().back(), '\n');
}

TEST(Logger, LevelFiltering) {
    std::ostringstream os;
    Logger log(&os, LogLevel::Warn);
    log.info("c", "dropped");
    log.debug("c", "dropped");
    EXPECT_TRUE(os.str().empty());
    log.warn("c", "kept");
    log.error("c", "kept2");
    EXPECT_NE(os.str().find("kept2"), std::string::npos);
    log.set_level(LogLevel::Off);
    os.str("");
    log.error("c", "silent");
    EXPECT_TRUE(os.str().empty());
}

TEST(Logger, EscapesControlCharacters) {
    EXPECT_EQ(json_escape(std::string("a\x01"
                                      "b")),
              "a\\u0001b");
    EXPECT_EQ(json_escape("tab\t"), "tab\\t");
}

TEST(Logger, ParseLevel) {
    EXPECT_EQ(parse_log_level("debug"), LogLevel::Debug);
    EXPECT_FALSE(parse_log_level("loud").has_value());
}

TEST(Logger, DefaultGlobalLoggerExists) {
    EXPECT_EQ(global_logger().level(), LogLevel::Info);
}
