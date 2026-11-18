#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>

#include "hcs/agent/event_parser.hpp"

using namespace hcs::agent;
using nlohmann::json;

TEST(EventParser, AgentStartAndEnd) {
    auto a = parse_agent_line(R"({"type":"agent_start"})");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, "agent_start");
    auto b = parse_agent_line(R"({"type":"agent_end"})");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].kind, "agent_end");
}

TEST(EventParser, ToolCallExtractsCodeOrCommandPayload) {
    auto a = parse_agent_line(
        R"j({"type":"tool_execution_start","toolName":"python_repl","toolCallId":"c1","args":{"code":"print(1)"}})j");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, "tool_call");
    EXPECT_EQ(a[0].tool, "python_repl");
    EXPECT_EQ(a[0].payload, "print(1)");
    EXPECT_EQ(a[0].data["id"], "c1");
    EXPECT_FALSE(a[0].data["truncated"].get<bool>());
    auto b = parse_agent_line(R"({"type":"tool_execution_start","toolName":"bash","args":{"command":"ls"}})");
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(b[0].payload, "ls");
}

TEST(EventParser, ToolCallPayloadIsCappedOnUtf8Boundary) {
    json j = {{"type", "tool_execution_start"},
              {"toolName", "t"},
              {"args", {{"code", std::string(100, 'x') + "\xE2\x82\xAC"}}}};
    auto a = parse_agent_line(j.dump(), 101);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].payload.size(), 100u);  // euro sign (3 bytes) not split
    EXPECT_TRUE(a[0].data["truncated"].get<bool>());
    EXPECT_EQ(a[0].data["bytes"].get<std::size_t>(), 103u);
}

TEST(EventParser, ToolResultJoinsTextPartsAndReadsDuration) {
    const char* line =
        R"({"type":"tool_execution_end","toolName":"bash","toolCallId":"c2","isError":true,)"
        R"("result":{"content":[{"type":"text","text":"ab"},{"type":"image"},{"type":"text","text":"cd"}],)"
        R"("details":{"durationMs":42}}})";
    auto a = parse_agent_line(line);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, "tool_result");
    EXPECT_EQ(a[0].data["text"], "abcd");
    EXPECT_FALSE(a[0].data["ok"].get<bool>());
    EXPECT_EQ(a[0].data["duration_ms"], 42);
}

TEST(EventParser, ToolResultWithoutResultIsEmptyOkEvent) {
    auto a = parse_agent_line(R"({"type":"tool_execution_end"})");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].data["text"], "");
    EXPECT_TRUE(a[0].data["ok"].get<bool>());
    EXPECT_EQ(a[0].data["duration_ms"], -1);
}

TEST(EventParser, TurnEndYieldsAssistantTextAndUsage) {
    const char* line =
        R"({"type":"turn_end","message":{"role":"assistant","content":"hello","stopReason":"stop",)"
        R"("usage":{"input":7,"output":3}}})";
    auto a = parse_agent_line(line);
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].kind, "assistant_text");
    EXPECT_EQ(a[0].data["text"], "hello");
    EXPECT_EQ(a[1].kind, "usage");
    EXPECT_EQ(a[1].data["input_tokens"], 7);
    EXPECT_EQ(a[1].data["output_tokens"], 3);
    EXPECT_EQ(a[1].data["stop_reason"], "stop");
}

TEST(EventParser, TurnEndFromNonAssistantOrWithoutMessageIsDropped) {
    EXPECT_TRUE(parse_agent_line(R"({"type":"turn_end","message":{"role":"user","content":"x"}})").empty());
    EXPECT_TRUE(parse_agent_line(R"({"type":"turn_end"})").empty());
    EXPECT_TRUE(parse_agent_line(R"({"type":"turn_end","message":"str"})").empty());
}

TEST(EventParser, StreamingDeltasAndUnknownTypesProduceNothing) {
    EXPECT_TRUE(parse_agent_line(R"({"type":"message_update","delta":"x"})").empty());
    EXPECT_TRUE(parse_agent_line(R"({"type":"something_new"})").empty());
}

TEST(EventParser, NonJsonAndStructurallyWrongLinesProduceNothing) {
    for (const char* line :
         {"", "plain log line", "[1,2]", "\"str\"", "42", "null", "{", "{\"type\":", "{}", "{\"type\":5}",
          "{\"type\":null}", "{\"type\":[\"agent_start\"]}", "  {\"type\":\"agent_start\"}"})
        EXPECT_TRUE(parse_agent_line(line).empty()) << line;
}

TEST(EventParser, WrongFieldTypesNeverThrow) {
    const char* lines[] = {
        R"({"type":"tool_execution_start","toolName":5,"toolCallId":[],"args":"nope"})",
        R"({"type":"tool_execution_start","toolName":null,"args":{"code":12,"command":{}}})",
        R"({"type":"tool_execution_start","args":null})",
        R"({"type":"tool_execution_end","toolName":{},"toolCallId":1,"isError":"yes","result":[]})",
        R"({"type":"tool_execution_end","result":{"content":{"a":1},"details":"x"}})",
        R"({"type":"tool_execution_end","result":{"content":[1,null,{"type":7,"text":"z"},{"type":"text","text":9}],)"
        R"("details":{"durationMs":"slow"}}})",
        R"({"type":"tool_execution_end","result":{"content":[],"details":{"durationMs":1e300}}})",
        R"({"type":"tool_execution_end","result":{"content":[],"details":{"durationMs":-5.5}}})",
        R"({"type":"turn_end","message":{"role":1,"content":2,"usage":{"input":"a","output":[]},"stopReason":{}}})",
        R"({"type":"turn_end","message":{"role":"assistant","content":[{"type":"text","text":"t"}],"usage":3}})",
        R"({"type":"turn_end","message":{"role":"assistant","usage":{"input":1e30,"output":-1}}})",
    };
    for (const char* line : lines) {
        EXPECT_NO_THROW({ (void)parse_agent_line(line); }) << line;
    }
}

TEST(EventParser, WrongTypedUsageFallsBackToZero) {
    auto a = parse_agent_line(
        R"({"type":"turn_end","message":{"role":"assistant","usage":{"input":"a","output":[]}}})");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, "usage");
    EXPECT_EQ(a[0].data["input_tokens"], 0);
    EXPECT_EQ(a[0].data["output_tokens"], 0);
}

TEST(EventParser, DeeplyNestedAndHugeLinesAreSafe) {
    std::string deep(200000, '[');
    deep += std::string(200000, ']');
    EXPECT_NO_THROW({ EXPECT_TRUE(parse_agent_line(deep).empty()); });
    const std::string deep_obj =
        R"({"type":"agent_start","x":)" + std::string(100000, '[') + std::string(100000, ']') + "}";
    EXPECT_NO_THROW({ (void)parse_agent_line(deep_obj); });
    const std::string huge = R"({"type":"turn_end","message":{"role":"assistant","content":")" +
                             std::string(5U * 1024U * 1024U, 'a') + R"("}})";
    auto a = parse_agent_line(huge, 1024);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].data["text"].get<std::string>().size(), 1024u);
    EXPECT_TRUE(a[0].data["truncated"].get<bool>());
}

TEST(EventParser, InvalidUtf8AndControlBytesNeverThrow) {
    const std::string bad = std::string(R"({"type":"turn_end","message":{"role":"assistant","content":")") +
                            "\xFF\xFE\x80\x01" + R"("}})";
    EXPECT_NO_THROW({ (void)parse_agent_line(bad); });
    const std::string with_nul = std::string("{\"type\":\"agent_start\"}") + '\0' + "junk";
    EXPECT_NO_THROW({ (void)parse_agent_line(with_nul); });
}

TEST(EventParser, RandomMutationsOfValidLinesNeverThrow) {
    const std::string seeds[] = {
        R"j({"type":"tool_execution_start","toolName":"python_repl","toolCallId":"c1","args":{"code":"print(1)"}})j",
        R"({"type":"tool_execution_end","toolName":"b","result":{"content":[{"type":"text","text":"ok"}],"details":{"durationMs":3}}})",
        R"({"type":"turn_end","message":{"role":"assistant","content":"x","usage":{"input":1,"output":2},"stopReason":"stop"}})"};
    const std::string alphabet = "{}[]\":,0a";
    std::mt19937 rng(12345);
    for (const auto& seed : seeds) {
        for (int i = 0; i < 400; ++i) {
            std::string m = seed;
            const int edits = 1 + static_cast<int>(rng() % 4);
            for (int e = 0; e < edits && !m.empty(); ++e) {
                const std::size_t pos = rng() % m.size();
                switch (rng() % 3) {
                    case 0:
                        m[pos] = static_cast<char>(rng() % 256);
                        break;
                    case 1:
                        m.erase(pos, 1 + rng() % 4);
                        break;
                    default:
                        m.insert(pos, 1, alphabet[rng() % alphabet.size()]);
                        break;
                }
            }
            EXPECT_NO_THROW({ (void)parse_agent_line(m); });
        }
    }
}
