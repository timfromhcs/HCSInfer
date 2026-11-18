#include "hcs/agent/agent_redact.hpp"

#include <algorithm>
#include <cctype>

namespace hcs::agent {

namespace {

bool is_tok(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '=' || c == '+' || c == '/';
}

// Replaces the run of token characters that follows `prefix` (case-sensitive) when it has >= min_len
// characters.
void redact_prefixed(std::string& s, std::string_view prefix, std::size_t min_len) {
    std::size_t pos = 0;
    while ((pos = s.find(prefix, pos)) != std::string::npos) {
        std::size_t end = pos + prefix.size();
        while (end < s.size() && is_tok(static_cast<unsigned char>(s[end]))) ++end;
        if (end - (pos + prefix.size()) >= min_len) {
            s.replace(pos, end - pos, "[redacted]");
            pos += 10;
        } else {
            pos = end;
        }
    }
}

void redact_ci_prefixed(std::string& s, std::string_view prefix, std::size_t min_len) {
    std::string low(s);
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t pos = 0;
    while ((pos = low.find(prefix, pos)) != std::string::npos) {
        std::size_t end = pos + prefix.size();
        while (end < s.size() && is_tok(static_cast<unsigned char>(s[end]))) ++end;
        if (end - (pos + prefix.size()) >= min_len) {
            s.replace(pos, end - pos, "[redacted]");
            low.replace(pos, end - pos, "[redacted]");
            pos += 10;
        } else {
            pos = end;
        }
    }
}

}  // namespace

std::string redact_secrets(std::string_view text, const std::vector<std::string>& literals) {
    std::string s(text);
    for (const auto& lit : literals) {
        if (lit.size() < 8)
            continue;  // too short to be a meaningful secret; avoids shredding ordinary text
        std::size_t pos = 0;
        while ((pos = s.find(lit, pos)) != std::string::npos) {
            s.replace(pos, lit.size(), "[redacted]");
            pos += 10;
        }
    }
    redact_ci_prefixed(s, "bearer ", 12);
    for (const std::string_view p : {"sk-", "ghp_", "gho_", "github_pat_", "hf_", "AKIA", "xoxb-", "xoxp-"})
        redact_prefixed(s, p, 12);
    return s;
}

std::string cap_utf8(std::string_view text, std::size_t max_bytes, bool& truncated) {
    truncated = false;
    if (text.size() <= max_bytes)
        return std::string(text);
    truncated = true;
    std::size_t keep = max_bytes;
    while (keep > 0 && (static_cast<unsigned char>(text[keep]) & 0xC0U) == 0x80U) --keep;
    return std::string(text.substr(0, keep));
}

}  // namespace hcs::agent
