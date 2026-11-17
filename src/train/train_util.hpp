#pragma once
// Internal helpers shared by the HCS Train sources (not installed, not part of the public ABI).
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace hcs::train::detail {

inline std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

inline bool is_hex64(std::string_view s) {
    if (s.size() != 64) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

inline bool is_reserved_device_name(std::string_view lower_name) {
    static constexpr std::string_view kFixed[] = {"con", "prn", "aux", "nul"};
    for (const auto n : kFixed) {
        if (lower_name == n) {
            return true;
        }
    }
    if (lower_name.size() == 4 && (lower_name.substr(0, 3) == "com" || lower_name.substr(0, 3) == "lpt")) {
        return lower_name[3] >= '1' && lower_name[3] <= '9';
    }
    return false;
}

// A safe single path component: `[A-Za-z0-9._-]`, must start alphanumeric, no "..", not a Windows device
// name. Used for evaluation ids and adapter ids (which become directory names).
inline bool valid_identifier(std::string_view s, std::size_t max_len) {
    if (s.empty() || s.size() > max_len) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(s.front());
    if (!std::isalnum(first)) {
        return false;
    }
    if (!std::all_of(s.begin(), s.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; })) {
        return false;
    }
    if (s.back() == '.' || s.find("..") != std::string_view::npos) {
        return false;
    }
    return !is_reserved_device_name(lower(s));
}

inline bool valid_adapter_id(std::string_view s) {
    return valid_identifier(s, 64);
}

inline bool valid_eval_id(std::string_view s) {
    return valid_identifier(s, 128);
}

// Maximum bracket nesting outside string literals: a cheap guard so hostile JSON never reaches the
// recursive parser with a depth that could exhaust the stack (same technique as DatasetValidator).
inline std::size_t json_depth(std::string_view s) {
    std::size_t depth = 0;
    std::size_t max_depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : s) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            max_depth = std::max(max_depth, ++depth);
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return max_depth;
}

inline constexpr std::size_t kMaxJsonDepth = 32;

}  // namespace hcs::train::detail
