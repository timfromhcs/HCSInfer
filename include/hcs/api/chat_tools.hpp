#pragma once
// OpenAI-style tool calling for POST /v1/chat/completions.
//
// Scope (what is real): request validation for `tools` / `tool_choice` / assistant `tool_calls` / `tool`
// messages, rendering of those into the model's ChatML prompt (the XML tool format of the LFM3 / Qwen3-Coder
// chat templates: `<tools>` JSON lines in the system turn, `<tool_call><function=..><parameter=..>` in
// assistant turns, `<tool_response>` in tool turns), and parsing of the generated text back into
// `tool_calls` (XML form and the Hermes JSON form) - for the complete text and incrementally for streaming.
// Not supported (rejected with 501, never silently ignored): `tool_choice` "required" or a named function,
// because nothing constrains the sampler to emit a call.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "hcs/result.hpp"

namespace hcs::api::chat_tools {

using json = nlohmann::json;

inline constexpr std::size_t kMaxTools = 64;
inline constexpr std::size_t kMaxToolsJsonBytes = 64 * 1024;
inline constexpr std::size_t kMaxToolCallsPerMessage = 16;

struct ToolDef {
    std::string name;
    std::string description;
    json parameters = json::object();  // JSON Schema object (types are used to coerce XML parameter values)
};

struct ToolCall {
    std::string id;
    std::string name;
    json arguments = json::object();
};

// How `tool_choice` was resolved. Auto: the model may answer or call. None: tools are hidden from the model.
enum class ToolMode { Off, Auto, None };

struct ToolsRequest {
    ToolMode mode = ToolMode::Off;
    std::vector<ToolDef> tools;
};

struct ParsedOutput {
    std::string content;  // text outside the tool-call blocks (trimmed at the ends)
    std::vector<ToolCall> calls;
};

// Validates `tools` and `tool_choice` of a request body (already schema-checked for shape). Errors carry
// ErrorCode::InvalidArgument (bad shape) or ErrorCode::Unsupported (`tool_choice` required/named,
// `functions`).
Result<ToolsRequest> parse_tools_request(const json& body);

// The `<tools>` block appended to the system turn: one compact JSON object per tool.
std::string render_tools_block(const std::vector<ToolDef>& tools);

// Assistant history: an OpenAI `tool_calls` array -> the XML tool-call text the model itself would have
// produced. `arguments` may be a JSON-encoded string (OpenAI) or an object. Errors: InvalidArgument.
Result<std::string> render_assistant_tool_calls(const json& tool_calls);

// A `tool` message body wrapped like the chat template does (`<tool_response>\n...\n</tool_response>`).
std::string render_tool_response(const std::string& content);

// Complete generation -> content + tool calls. `ids` are generated as "call_<seq>_<n>" from `seq`.
ParsedOutput parse_output(std::string_view text, const std::vector<ToolDef>& tools, std::uint64_t seq);

// Incremental splitter for SSE: feed decoded pieces, get back the text that is safe to stream as `content`
// (anything that could be the start of a `<tool_call>` block is held back until it is decided).
class StreamSplitter {
public:
    StreamSplitter(const std::vector<ToolDef>& tools, std::uint64_t seq) : tools_(tools), seq_(seq) {}
    // Returns the content to emit now (possibly empty).
    std::string feed(std::string_view piece);
    // End of generation: flushes held-back text and parses the buffered tool-call region.
    ParsedOutput finish();
    bool in_tool_call() const { return in_call_; }

private:
    const std::vector<ToolDef>& tools_;
    std::uint64_t seq_;
    std::string pending_;  // not yet emitted (possible tag prefix) while outside a call
    std::string call_;     // everything from the first "<tool_call>" on
    std::string emitted_;  // content already streamed
    bool in_call_ = false;
};

}  // namespace hcs::api::chat_tools
