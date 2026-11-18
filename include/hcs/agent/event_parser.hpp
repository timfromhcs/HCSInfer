#pragma once
// Maps one line of `prime-agent --mode json` output (ADR-011, docs/research/prime-agent.md) to HCS agent
// events. Only the events HCS needs are kept (assistant text, tool call, tool result, usage, end); streaming
// deltas and internal harness messages are dropped. Unknown or malformed lines produce no event, never an
// exception.
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hcs::agent {

struct ParsedEvent {
    std::string kind;     // agent_start | assistant_text | tool_call | tool_result | usage | agent_end
    nlohmann::json data;  // kind specific, already size-capped
    std::string tool;     // tool_call only: tool name
    std::string payload;  // tool_call only: code/command text (capped to max_text, used by ToolAudit)
};

std::vector<ParsedEvent> parse_agent_line(std::string_view line,
                                          std::size_t max_text_bytes = std::size_t{16} * 1024);

}  // namespace hcs::agent
