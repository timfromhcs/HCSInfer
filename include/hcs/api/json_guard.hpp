#pragma once
// Cheap pre-parse guard against pathologically nested JSON (AGENTS.md 19.1/19.3). nlohmann::json copies,
// compares and destroys values recursively, so a few thousand nested brackets can exhaust the stack even when
// the parse itself succeeds (M10 audit D1). Callers reject deep input BEFORE parsing it.
#include <string_view>

namespace hcs::api {

// Maximum bracket nesting outside of strings. O(n), no allocation.
inline bool json_depth_ok(std::string_view s, int max_depth) {
    int depth = 0;
    bool in_str = false, esc = false;
    for (char c : s) {
        if (in_str) {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                in_str = false;
            continue;
        }
        if (c == '"')
            in_str = true;
        else if (c == '{' || c == '[') {
            if (++depth > max_depth)
                return false;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return true;
}

// Depth cap used for every untrusted JSON document (HTTP bodies, MCP lines). Real requests nest < 10 levels.
inline constexpr int kMaxJsonDepth = 64;

}  // namespace hcs::api
