#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "hcs/train/chat_template.hpp"

using namespace hcs::train;

namespace {

// Golden strings produced by tools/train/jinja_reference.py (the GGUF's own Jinja template rendered with
// Jinja2).
const char* kToolsRecord =
    R"({"id":"a","tools":[{"type":"function","function":{"name":"get_weather","description":"Get the weather.","parameters":{"type":"object","properties":{"city":{"type":"string"},"unit":{"type":"string","enum":["celsius","fahrenheit"]}},"required":["city"]}}}],"messages":[{"role":"system","content":"You are helpful."},{"role":"user","content":"Weather in Paris and Rome?"},{"role":"assistant","content":null,"tool_calls":[{"function":{"name":"get_weather","arguments":{"city":"Paris","unit":"celsius"}}},{"function":{"name":"get_weather","arguments":"{\"city\": \"Rome\", \"n\": 3, \"ok\": true, \"l\": [1, \"x\"], \"z\": null}"}}]},{"role":"tool","tool_call_id":"1","content":"{\"t\": 20}"},{"role":"tool","tool_call_id":"2","content":"sunny"},{"role":"assistant","content":"Paris 20C, Rome sunny."}]})";

const std::string kToolsGolden =
    "<|startoftext|><|im_start|>system\nYou are helpful.\n\n<tools>\n{\"name\": \"get_weather\", "
    "\"description\": "
    "\"Get the weather.\", \"parameters\": {\"type\": \"object\", \"properties\": {\"city\": {\"type\": "
    "\"string\"}, "
    "\"unit\": {\"type\": \"string\", \"enum\": [\"celsius\", \"fahrenheit\"]}}, \"required\": "
    "[\"city\"]}}\n</tools>"
    "<|im_end|>\n<|im_start|>user\nWeather in Paris and "
    "Rome?<|im_end|>\n<|im_start|>assistant\n<think></think>"
    "<tool_call>\n<function=get_weather>\n<parameter=city>\nParis\n</parameter>\n<parameter=unit>\ncelsius\n"
    "</parameter>\n</function>\n</tool_call>\n\n<tool_call>\n<function=get_weather>\n<parameter=city>\nRome\n"
    "</parameter>\n<parameter=n>\n3\n</parameter>\n<parameter=ok>\ntrue\n</parameter>\n<parameter=l>\n[1, "
    "\"x\"]\n"
    "</parameter>\n<parameter=z>\nnull\n</parameter>\n</function>\n</tool_call><|im_end|>\n<|im_start|>tool\n"
    "<tool_response>\n{\"t\": 20}\n</tool_response><|im_end|>\n<|im_start|>tool\n<tool_response>\nsunny\n"
    "</tool_response><|im_end|>\n<|im_start|>assistant\n<think></think>Paris 20C, Rome sunny.<|im_end|>\n";

const char* kPlainRecord =
    R"({"id":"b","messages":[{"role":"user","content":"Hi"},{"role":"assistant","content":"Hello! é"},{"role":"user","content":"Bye"},{"role":"assistant","content":"Bye."}]})";
const std::string kPlainGolden =
    "<|startoftext|><|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n<think></think>Hello! "
    "\xc3\xa9<|im_end|>\n"
    "<|im_start|>user\nBye<|im_end|>\n<|im_start|>assistant\n<think></think>Bye.<|im_end|>\n";

OJson parse(const char* s) {
    return OJson::parse(s);
}

}  // namespace

TEST(ChatTemplate, MatchesJinjaGoldenWithToolsAndParallelCalls) {
    const OJson rec = parse(kToolsRecord);
    const auto r = render_lfm3(rec["messages"], rec["tools"], false);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value(), kToolsGolden);
}

TEST(ChatTemplate, MatchesJinjaGoldenForPlainMultiTurnUtf8) {
    const OJson rec = parse(kPlainRecord);
    const auto r = render_lfm3(rec["messages"], OJson::array(), false);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value(), kPlainGolden);
}

TEST(ChatTemplate, GenerationPromptEndsWithEmptyThinkBlock) {
    const OJson msgs = parse(R"([{"role":"user","content":"Hi"}])");
    const auto r = render_lfm3(msgs, OJson::array(), true);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(),
              "<|startoftext|><|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n<think></think>");
}

TEST(ChatTemplate, ToolSchemaKeyOrderIsPreserved) {
    const OJson tools = parse(R"([{"type":"function","function":{"parameters":{"b":1,"a":2},"name":"f"}}])");
    const OJson msgs = parse(R"([{"role":"user","content":"x"}])");
    const auto r = render_lfm3(msgs, tools, false);
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value().find("{\"parameters\": {\"b\": 1, \"a\": 2}, \"name\": \"f\"}"), std::string::npos)
        << r.value();
}

TEST(ChatTemplate, RejectsUnsupportedInputInsteadOfRenderingItDifferently) {
    const OJson think =
        parse(R"([{"role":"user","content":"x"},{"role":"assistant","content":"<think>a</think>b"}])");
    EXPECT_EQ(render_lfm3(think, OJson::array(), false).error().code, hcs::ErrorCode::Unsupported);
    const OJson parts = parse(R"([{"role":"user","content":[{"type":"text","text":"x"}]}])");
    EXPECT_EQ(render_lfm3(parts, OJson::array(), false).error().code, hcs::ErrorCode::Unsupported);
    const OJson merged = parse(
        R"([{"role":"user","content":"x"},{"role":"tool","tool_call_id":"1","content":"a"},{"role":"tool","tool_call_id":"1","content":"b"}])");
    EXPECT_EQ(render_lfm3(merged, OJson::array(), false).error().code, hcs::ErrorCode::Unsupported);
    EXPECT_FALSE(render_lfm3(OJson::object(), OJson::array(), false).ok());
    const OJson badargs =
        parse(R"([{"role":"assistant","tool_calls":[{"function":{"name":"f","arguments":"[1]"}}]}])");
    EXPECT_FALSE(render_lfm3(badargs, OJson::array(), false).ok());
}

TEST(SftExamples, SplitsPerAssistantTurnAtTheGenerationPrompt) {
    const OJson rec = parse(kToolsRecord);
    const auto ex = to_sft_examples(rec);
    ASSERT_TRUE(ex.ok()) << ex.error().message;
    ASSERT_EQ(ex.value().size(), 2u);
    EXPECT_EQ(ex.value()[0].id, "a#2");
    EXPECT_EQ(ex.value()[1].id, "a#5");
    // prompt + completion + newline reproduces the full render up to that turn
    EXPECT_EQ(ex.value()[1].prompt + ex.value()[1].completion + "\n", kToolsGolden);
    EXPECT_EQ(ex.value()[1].completion, "Paris 20C, Rome sunny.<|im_end|>");
    EXPECT_TRUE(ex.value()[0].completion.rfind("<tool_call>\n<function=get_weather>", 0) == 0);
    EXPECT_EQ(ex.value()[1].prompt.substr(ex.value()[1].prompt.size() - 37),
              "<|im_start|>assistant\n<think></think>");
}

TEST(SftExamples, RecordWithoutAssistantTurnIsAnError) {
    const OJson rec = parse(R"({"id":"x","messages":[{"role":"user","content":"hi"}]})");
    EXPECT_FALSE(to_sft_examples(rec).ok());
    EXPECT_FALSE(to_sft_examples(parse("[]")).ok());
}

TEST(TemplateJson, UsesPythonSeparatorsAndKeepsUnicode) {
    EXPECT_EQ(template_json(parse(R"({"a":[1,2,{"b":"é"}],"c":null,"d":true})")),
              "{\"a\": [1, 2, {\"b\": \"\xc3\xa9\"}], \"c\": null, \"d\": true}");
}

TEST(ChatTemplate, WronglyTypedFieldsNeverThrow) {
    for (
        const char* msgs :
        {R"([{"role":5,"content":"x"}])", R"([{"role":"system","content":"s"},{"role":null}])",
         R"([{"role":"tool","tool_call_id":7,"content":"a"},{"role":"tool","tool_call_id":7,"content":"b"}])",
         R"([{"role":"assistant","content":{"a":1}}])",
         R"([{"role":"assistant","tool_calls":[{"function":{"name":3}}]}])",
         R"([{"role":"assistant","tool_calls":"nope"}])", R"([{"role":"assistant","tool_calls":[7]}])",
         R"([1,"two",null])"}) {
        EXPECT_NO_THROW({ (void)render_lfm3(parse(msgs), OJson::array(), true); }) << msgs;
    }
    EXPECT_NO_THROW({
        (void)render_lfm3(parse(R"([{"role":"user","content":"x"}])"), parse(R"([1,"x",{"function":5}])"),
                          false);
    });
    for (const char* rec : {R"({"id":5,"messages":[{"role":"assistant","content":"a"}]})",
                            R"({"id":"x","messages":[{"role":["assistant"],"content":"a"}]})",
                            R"({"messages":"nope"})", R"([])", R"("s")", R"(null)"}) {
        EXPECT_NO_THROW({ (void)to_sft_examples(parse(rec)); }) << rec;
    }
}

TEST(ChatTemplate, WronglyTypedIdIsTreatedAsEmpty) {
    const auto ex = to_sft_examples(
        parse(R"({"id":5,"messages":[{"role":"user","content":"q"},{"role":"assistant","content":"a"}]})"));
    ASSERT_TRUE(ex.ok());
    EXPECT_EQ(ex.value()[0].id, "#1");
}

TEST(ChatTemplate, ExcessiveJsonNestingIsRejectedWithoutStackOverflow) {
    // Built by parsing (iterative) - copying such a value would itself recurse, which is exactly the hazard.
    const std::string deep = std::string(100000, '[') + std::string(100000, ']');
    const OJson rec = OJson::parse(
        R"({"id":"d","messages":[{"role":"user","content":"q"},{"role":"assistant","tool_calls":[{"function":{"name":"f","arguments":{"x":)" +
        deep + "}}}]}]}");
    const auto r = render_lfm3(rec["messages"], OJson::array(), false);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, hcs::ErrorCode::InvalidArgument);
    const auto ex = to_sft_examples(rec);
    ASSERT_FALSE(ex.ok());
    EXPECT_EQ(ex.error().code, hcs::ErrorCode::InvalidArgument);
    const OJson args = OJson::parse(deep);
    EXPECT_NO_THROW({ (void)template_json(args); });  // direct callers are bounded as well
    // moderate nesting still renders
    const OJson ok_args = OJson::parse(R"({"a":[[[[[1]]]]]})");
    EXPECT_NE(template_json(ok_args).find("[[[[[1]]]]]"), std::string::npos);
}

TEST(ChatTemplate, ToolCallsWithoutArgumentsRenderEmptyParameterBlock) {
    const OJson msgs = parse(
        R"([{"role":"assistant","tool_calls":[{"function":{"name":"ping"}},{"name":"bare","arguments":"{}"}]}])");
    const auto r = render_lfm3(msgs, OJson::array(), false);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_NE(r.value().find(
                  "<function=ping>\n</function>\n</tool_call>\n\n<tool_call>\n<function=bare>\n</function>"),
              std::string::npos)
        << r.value();
}
