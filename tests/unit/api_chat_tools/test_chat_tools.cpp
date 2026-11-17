// Unit tests for OpenAI-style tool calling: request validation, prompt rendering, output parsing, streaming.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hcs/api/chat_tools.hpp"

using namespace hcs;
using namespace hcs::api::chat_tools;

namespace {

json weather_tools() {
    return json::parse(R"({"tools":[{"type":"function","function":{"name":"get_weather",
      "description":"Get the weather","parameters":{"type":"object","properties":{
      "city":{"type":"string"},"days":{"type":"integer"},"metric":{"type":"boolean"}},"required":["city"]}}}]})");
}

std::vector<ToolDef> defs() {
    auto r = parse_tools_request(weather_tools());
    return r.value().tools;
}

}  // namespace

TEST(ChatToolsRequest, NoToolsIsOff) {
    auto r = parse_tools_request(json::parse(R"({"messages":[]})"));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().mode, ToolMode::Off);
}

TEST(ChatToolsRequest, ParsesToolsAndDefaultsToAuto) {
    auto r = parse_tools_request(weather_tools());
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().mode, ToolMode::Auto);
    ASSERT_EQ(r.value().tools.size(), 1u);
    EXPECT_EQ(r.value().tools[0].name, "get_weather");
}

TEST(ChatToolsRequest, ToolChoiceNoneAndAuto) {
    json b = weather_tools();
    b["tool_choice"] = "none";
    EXPECT_EQ(parse_tools_request(b).value().mode, ToolMode::None);
    b["tool_choice"] = "auto";
    EXPECT_EQ(parse_tools_request(b).value().mode, ToolMode::Auto);
}

TEST(ChatToolsRequest, RequiredAndNamedChoiceAreUnsupported) {
    json b = weather_tools();
    b["tool_choice"] = "required";
    auto r = parse_tools_request(b);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    b["tool_choice"] = json::parse(R"({"type":"function","function":{"name":"get_weather"}})");
    r = parse_tools_request(b);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
}

TEST(ChatToolsRequest, LegacyFunctionsAreUnsupported) {
    auto r = parse_tools_request(json::parse(R"({"functions":[]})"));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
}

TEST(ChatToolsRequest, RejectsMalformedDefinitions) {
    for (const char* bad : {R"({"tools":"x"})", R"({"tools":[1]})", R"({"tools":[{"type":"function"}]})",
                            R"({"tools":[{"type":"function","function":{"name":"bad name"}}]})",
                            R"({"tools":[{"type":"function","function":{"name":""}}]})",
                            R"({"tools":[{"type":"function","function":{"name":"a","parameters":3}}]})",
                            R"({"tool_choice":"auto"})",
                            R"({"tools":[{"type":"function","function":{"name":"a"}},)"
                            R"({"type":"function","function":{"name":"a"}}]})"}) {
        auto r = parse_tools_request(json::parse(bad));
        ASSERT_FALSE(r.ok()) << bad;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << bad;
    }
}

TEST(ChatToolsRequest, RejectsTooManyAndTooLarge) {
    json b = {{"tools", json::array()}};
    for (std::size_t i = 0; i <= kMaxTools; ++i)
        b["tools"].push_back({{"type", "function"}, {"function", {{"name", "f" + std::to_string(i)}}}});
    EXPECT_FALSE(parse_tools_request(b).ok());
    json big = {
        {"tools",
         json::array(
             {json{{"type", "function"},
                   {"function", {{"name", "f"}, {"description", std::string(kMaxToolsJsonBytes, 'x')}}}}})}};
    EXPECT_FALSE(parse_tools_request(big).ok());
}

TEST(ChatToolsRender, ToolsBlockUsesPythonJsonLayout) {
    const std::string b = render_tools_block(defs());
    EXPECT_EQ(
        b.rfind(
            "<tools>\n{\"name\": \"get_weather\", \"description\": \"Get the weather\", \"parameters\": {",
            0),
        0u);
    EXPECT_NE(b.find("</tools>"), std::string::npos);
}

TEST(ChatToolsRender, AssistantToolCallsAreXml) {
    auto r = render_assistant_tool_calls(json::parse(
        R"([{"id":"c1","type":"function","function":{"name":"get_weather","arguments":"{\"city\":\"Paris\",\"days\":3,\"metric\":true}"}}])"));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(),
              "<tool_call>\n<function=get_weather>\n<parameter=city>\nParis\n</parameter>\n"
              "<parameter=days>\n3\n</parameter>\n<parameter=metric>\ntrue\n</parameter>\n</function>\n</"
              "tool_call>");
}

TEST(ChatToolsRender, RejectsBadAssistantToolCalls) {
    EXPECT_FALSE(render_assistant_tool_calls(json::parse(R"({"a":1})")).ok());
    EXPECT_FALSE(
        render_assistant_tool_calls(json::parse(R"([{"function":{"name":"x","arguments":"not json"}}])"))
            .ok());
    EXPECT_FALSE(render_assistant_tool_calls(json::parse(R"([{"function":{"name":"x y"}}])")).ok());
    EXPECT_FALSE(
        render_assistant_tool_calls(json::parse(R"([{"function":{"name":"x","arguments":"[1]"}}])")).ok());
}

TEST(ChatToolsRender, ToolResponseWrapsOnce) {
    EXPECT_EQ(render_tool_response("21 C"), "<tool_response>\n21 C\n</tool_response>");
    EXPECT_EQ(render_tool_response("<tool_response>\nx\n</tool_response>"),
              "<tool_response>\nx\n</tool_response>");
}

TEST(ChatToolsParse, PlainTextHasNoCalls) {
    auto p = parse_output("  Hello there.  ", defs(), 1);
    EXPECT_TRUE(p.calls.empty());
    EXPECT_EQ(p.content, "Hello there.");
}

TEST(ChatToolsParse, XmlCallIsTypedBySchema) {
    const std::string t =
        "Let me check.\n\n<tool_call>\n<function=get_weather>\n<parameter=city>\n123\n</parameter>\n"
        "<parameter=days>\n3\n</parameter>\n<parameter=metric>\ntrue\n</parameter>\n</function>\n</"
        "tool_call>";
    auto p = parse_output(t, defs(), 7);
    ASSERT_EQ(p.calls.size(), 1u);
    EXPECT_EQ(p.content, "Let me check.");
    EXPECT_EQ(p.calls[0].name, "get_weather");
    EXPECT_EQ(p.calls[0].id, "call_7_0");
    EXPECT_EQ(p.calls[0].arguments["city"], "123");  // schema says string: stays a string
    EXPECT_EQ(p.calls[0].arguments["days"], 3);
    EXPECT_EQ(p.calls[0].arguments["metric"], true);
}

TEST(ChatToolsParse, MultipleCallsAndHermesJson) {
    const std::string t =
        "<tool_call>\n{\"name\": \"get_weather\", \"arguments\": {\"city\": \"Rome\"}}\n</tool_call>"
        "<tool_call>\n<function=get_weather>\n<parameter=city>\nOslo\n</parameter>\n</function>\n</"
        "tool_call>";
    auto p = parse_output(t, defs(), 1);
    ASSERT_EQ(p.calls.size(), 2u);
    EXPECT_EQ(p.calls[0].arguments["city"], "Rome");
    EXPECT_EQ(p.calls[1].arguments["city"], "Oslo");
    EXPECT_EQ(p.calls[1].id, "call_1_1");
    EXPECT_TRUE(p.content.empty());
}

TEST(ChatToolsParse, UnparseableBlockStaysVisible) {
    auto p = parse_output("<tool_call>\ngarbage\n</tool_call>", defs(), 1);
    EXPECT_TRUE(p.calls.empty());
    EXPECT_NE(p.content.find("garbage"), std::string::npos);
}

TEST(ChatToolsParse, TruncatedBlockWithoutFunctionEndIsKeptAsText) {
    auto p = parse_output("<tool_call>\n<function=get_weather>\n<parameter=city>\nPar", defs(), 1);
    EXPECT_TRUE(p.calls.empty());
    EXPECT_NE(p.content.find("Par"), std::string::npos);
}

TEST(ChatToolsStream, PlainTextStreamsWithoutLoss) {
    const auto d = defs();
    StreamSplitter s(d, 1);
    std::string out;
    for (const char* piece : {"Hel", "lo ", "wor", "ld", ".", " <", "b>"}) out += s.feed(piece);
    auto fin = s.finish();
    out += fin.content;
    EXPECT_EQ(out, "Hello world. <b>");
    EXPECT_TRUE(fin.calls.empty());
}

TEST(ChatToolsStream, HoldsBackTagPrefixAndParsesCall) {
    const auto d = defs();
    StreamSplitter s(d, 3);
    const std::string full =
        "Checking.\n\n<tool_call>\n<function=get_weather>\n<parameter=city>\nParis\n</parameter>\n"
        "</function>\n</tool_call>";
    std::string out;
    for (std::size_t i = 0; i < full.size(); i += 3) {
        const std::string e = s.feed(full.substr(i, 3));
        EXPECT_EQ(e.find("<tool"), std::string::npos) << "tag text leaked into content";
        out += e;
    }
    auto fin = s.finish();
    EXPECT_EQ(out, "Checking.");
    ASSERT_EQ(fin.calls.size(), 1u);
    EXPECT_EQ(fin.calls[0].arguments["city"], "Paris");
    EXPECT_TRUE(s.in_tool_call());
}

TEST(ChatToolsStream, UnusableCallRegionIsReturnedAsContent) {
    const auto d = defs();
    StreamSplitter s(d, 1);
    (void)s.feed("<tool_call>\n<function=get_weather>\n<parameter=city>\nPa");
    auto fin = s.finish();
    EXPECT_TRUE(fin.calls.empty());
    EXPECT_NE(fin.content.find("Pa"), std::string::npos);
}
