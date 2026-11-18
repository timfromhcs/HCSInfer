#pragma once
// LFM3 chat-template renderer for HCS Train (ADR-012).
//
// Reproduces, for the subset HCS trains on, the Jinja chat template that ships inside the LFM3-Omni GGUF
// (`tokenizer.chat_template`): system + <tools> block, user/assistant/tool turns, tool calls in the
// `<tool_call>\n<function=NAME>\n<parameter=ARG>\nVALUE\n</parameter>\n</function>\n</tool_call>` form, and
// the generation prompt `<|im_start|>assistant\n<think></think>`.
//
// Supported subset (anything else is rejected with Unsupported, never silently rendered differently):
//  - string message content only (no image/audio parts), no thinking text (`<think>` / `</think>` in
//  content),
//  - no consecutive `tool` messages sharing one tool_call_id (the template merges those),
//  - tool-call arguments as a JSON object, or a JSON string holding one (parsed first, as the template
//  demands).
// Key order of tool schemas and arguments is preserved (ordered JSON), like the template does.
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "hcs/result.hpp"

namespace hcs::train {

using OJson = nlohmann::ordered_json;

inline constexpr std::string_view kLfm3Bos = "<|startoftext|>";  // tokenizer.ggml.bos_token_id 124894

// Python-style compact JSON as Jinja's `tojson` prints it: ", " and ": " separators, UTF-8 kept as is.
std::string template_json(const OJson& v);

// Renders `messages` (OpenAI-style array) and optional `tools`. With `add_generation_prompt` the result ends
// with "<|im_start|>assistant\n<think></think>".
Result<std::string> render_lfm3(const OJson& messages, const OJson& tools, bool add_generation_prompt);

// One supervised example: loss is computed on `completion` only.
struct SftExample {
    std::string id;          // record id + "#<assistant turn index>"
    std::string prompt;      // everything up to and including the generation prompt
    std::string completion;  // assistant content + tool calls + "<|im_end|>"
};

// Splits a conversation record ({"messages": [...], "tools": [...]}) into one example per assistant turn.
// Internally verifies prompt + completion + "\n" equals the full render up to that turn.
Result<std::vector<SftExample>> to_sft_examples(const OJson& record);

}  // namespace hcs::train
