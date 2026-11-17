#include <gtest/gtest.h>

#include "hcs/api/json_schema.hpp"

using hcs::api::validate_json;
using nlohmann::json;

TEST(JsonSchema, TypesAndRequired) {
    const json s = json::parse(R"({"type":"object","required":["a"],"properties":{"a":{"type":"string"},"b":{"type":"integer"}}})");
    EXPECT_TRUE(validate_json(s, json::parse(R"({"a":"x"})")));
    EXPECT_FALSE(validate_json(s, json::parse(R"({})")));
    EXPECT_FALSE(validate_json(s, json::parse(R"({"a":1})")));
    EXPECT_FALSE(validate_json(s, json::parse(R"({"a":"x","b":1.5})")));
    EXPECT_FALSE(validate_json(s, json::parse(R"([])")));
}

TEST(JsonSchema, AdditionalPropertiesFalseRejectsUnknown) {
    const json s = json::parse(R"({"type":"object","properties":{"a":{"type":"string"}},"additionalProperties":false})");
    EXPECT_TRUE(validate_json(s, json::parse(R"({"a":"x"})")));
    auto r = validate_json(s, json::parse(R"({"a":"x","z":1})"));
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("unexpected property"), std::string::npos);
}

TEST(JsonSchema, NumericAndStringBounds) {
    const json s = json::parse(R"({"type":"object","properties":{"n":{"type":"integer","minimum":1,"maximum":10},"s":{"type":"string","maxLength":3,"minLength":1}}})");
    EXPECT_TRUE(validate_json(s, json::parse(R"({"n":10,"s":"abc"})")));
    EXPECT_FALSE(validate_json(s, json::parse(R"({"n":0})")));
    EXPECT_FALSE(validate_json(s, json::parse(R"({"n":11})")));
    EXPECT_FALSE(validate_json(s, json::parse(R"({"s":"abcd"})")));
    EXPECT_FALSE(validate_json(s, json::parse(R"({"s":""})")));
}

TEST(JsonSchema, ArraysEnumsAndAnyOf) {
    const json s = json::parse(R"({"type":"array","minItems":1,"maxItems":2,"items":{"type":"string","enum":["a","b"]}})");
    EXPECT_TRUE(validate_json(s, json::parse(R"(["a","b"])")));
    EXPECT_FALSE(validate_json(s, json::parse(R"([])")));
    EXPECT_FALSE(validate_json(s, json::parse(R"(["a","b","a"])")));
    EXPECT_FALSE(validate_json(s, json::parse(R"(["c"])")));
    const json any = json::parse(R"({"anyOf":[{"type":"string"},{"type":"array"}]})");
    EXPECT_TRUE(validate_json(any, json("x")));
    EXPECT_TRUE(validate_json(any, json::array()));
    EXPECT_FALSE(validate_json(any, json(5)));
}

TEST(JsonSchema, ErrorMessageCarriesPath) {
    const json s = json::parse(R"({"type":"object","properties":{"m":{"type":"array","items":{"type":"object","required":["r"]}}}})");
    auto r = validate_json(s, json::parse(R"({"m":[{"r":1},{}]})"));
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("/m/1"), std::string::npos);
    EXPECT_EQ(r.error().code, hcs::ErrorCode::InvalidArgument);
}

TEST(JsonSchema, UnsupportedKeywordNeverSilentlyWeakens) {
    const json s = json::parse(R"({"type":"string","pattern":"^a"})");
    EXPECT_FALSE(validate_json(s, json("abc")));  // rejected rather than ignored
}

TEST(JsonSchema, DeepSchemaIsBounded) {
    json s = json::object();
    json* cur = &s;
    for (int i = 0; i < 40; ++i) {
        (*cur)["items"] = json::object();
        (*cur)["type"] = "array";
        cur = &(*cur)["items"];
    }
    json v = json::array();
    json* c = &v;
    for (int i = 0; i < 40; ++i) { c->push_back(json::array()); c = &(*c)[0]; }
    EXPECT_FALSE(validate_json(s, v));
}
