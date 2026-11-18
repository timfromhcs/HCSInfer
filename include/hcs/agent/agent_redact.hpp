#pragma once
// Secret redaction for agent events and logs (AGENTS.md 0.2, 19.4): known literals (the session token) and
// common credential shapes are replaced by "[redacted]" before anything is stored, streamed or logged.
#include <string>
#include <string_view>
#include <vector>

namespace hcs::agent {

std::string redact_secrets(std::string_view text, const std::vector<std::string>& literals = {});

// Cuts `text` to at most `max_bytes` on a UTF-8 boundary; sets `truncated` when it did.
std::string cap_utf8(std::string_view text, std::size_t max_bytes, bool& truncated);

}  // namespace hcs::agent
