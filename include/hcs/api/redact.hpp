#pragma once
// Scrubs absolute local filesystem paths from error text before it leaves the process (ADR-007,
// AGENTS.md 19.4; M10 audit D6). The full text stays in the local log; API/MCP clients get "<path>/<file
// name>" so the failing file is still recognisable but the directory layout and user names are not disclosed.
//
// Recognised: Windows drive paths (C:\x, D:/x), UNC paths (\\server\share\x), \\?\ and \\.\ prefixed paths,
// and POSIX absolute paths under the usual user/system roots (/home, /Users, /tmp, /var, /mnt, ...). Ordinary
// text
// ("ratio 3:4", "see /v1/models", "1..4096") is left untouched.
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace hcs::api {

namespace redact_detail {

inline bool is_sep(char c) {
    return c == '\\' || c == '/';
}
inline bool alnum(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

// True when position i may begin a path token (start of text or after a non-word character).
inline bool boundary_before(std::string_view s, std::size_t i) {
    if (i == 0)
        return true;
    const char p = s[i - 1];
    return !alnum(p) && p != '_' && p != '.' && p != '-' && !is_sep(p);
}

inline bool starts_with_posix_root(std::string_view s, std::size_t i) {
    static constexpr std::string_view roots[] = {"/home/", "/Users/", "/tmp/", "/var/", "/mnt/", "/media/",
                                                 "/opt/",  "/root/",  "/etc/", "/usr/", "/srv/", "/private/"};
    for (auto r : roots)
        if (s.substr(i, r.size()) == r)
            return true;
    return false;
}

// Length of a path-start prefix that is NOT part of the user-visible directory (e.g. "\\?\"), 0 if none.
inline std::size_t verbatim_prefix(std::string_view s, std::size_t i) {
    if (s.size() >= i + 4 && s[i] == '\\' && s[i + 1] == '\\' && (s[i + 2] == '?' || s[i + 2] == '.') &&
        s[i + 3] == '\\')
        return 4;
    return 0;
}

inline bool is_drive_start(std::string_view s, std::size_t i) {
    return i + 2 < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && s[i + 1] == ':' &&
           is_sep(s[i + 2]);
}

inline bool is_unc_start(std::string_view s, std::size_t i) {
    return i + 2 < s.size() && s[i] == '\\' && s[i + 1] == '\\' && s[i + 2] != '?' && s[i + 2] != '.' &&
           s[i + 2] != '\\' && !is_sep(s[i + 2]);
}

// End (exclusive) of the path token that starts at `begin`; may contain spaces when what follows still looks
// like more path ("C:\Users\bob smith\x.gguf") but stops at ": ", quotes and "(code 2)"-style trailers.
inline std::size_t path_end(std::string_view s, std::size_t begin) {
    std::size_t j = begin;
    const std::size_t n = s.size();
    while (j < n) {
        const char c = s[j];
        if (c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' || c == '*' || c == '?' ||
            static_cast<unsigned char>(c) < 0x20)
            break;
        if (c == ':' && j > begin + 1 && (j + 1 >= n || s[j + 1] == ' '))
            break;  // "...x.gguf: reason"
        if (c == ',' && (j + 1 >= n || s[j + 1] == ' '))
            break;
        if (c == ' ') {
            std::size_t k = j + 1;
            while (k < n && s[k] != ' ' && s[k] != '"' && s[k] != '\'') ++k;
            const std::string_view tok = s.substr(j + 1, k - (j + 1));
            const bool has_sep =
                tok.find('\\') != std::string_view::npos || tok.find('/') != std::string_view::npos;
            if (tok.empty() || tok.front() == '(' || !has_sep)
                break;
        }
        ++j;
    }
    return j;
}

}  // namespace redact_detail

inline std::string redact_paths(std::string_view s) {
    using namespace redact_detail;
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        std::size_t begin = i;  // where the path text (without a verbatim prefix) starts
        bool hit = false;
        if (boundary_before(s, i)) {
            const std::size_t vp = verbatim_prefix(s, i);
            if (vp != 0 && (is_drive_start(s, i + vp) || s.substr(i + vp, 4) == "UNC\\")) {
                begin = i + vp;
                hit = true;
            } else if (is_drive_start(s, i) || is_unc_start(s, i) ||
                       (s[i] == '/' && starts_with_posix_root(s, i))) {
                hit = true;
            }
        }
        if (!hit) {
            out.push_back(s[i++]);
            continue;
        }
        std::size_t end = path_end(s, begin);
        while (end > begin && std::string_view(".,;:)]}").find(s[end - 1]) != std::string_view::npos) --end;
        const std::string_view region = s.substr(begin, end - begin);
        std::size_t last = std::string_view::npos;
        for (std::size_t k = 0; k < region.size(); ++k)
            if (is_sep(region[k]))
                last = k;
        const std::string_view base =
            last == std::string_view::npos ? std::string_view{} : region.substr(last + 1);
        out += "<path>";
        if (!base.empty()) {
            out.push_back('/');
            out.append(base);
        }
        i = end;
    }
    return out;
}

}  // namespace hcs::api
