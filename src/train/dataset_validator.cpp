#include "hcs/train/dataset_validator.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace hcs::train {

using nlohmann::json;

namespace {

constexpr std::size_t kMaxJsonDepth = 64;

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool is_blank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

// Maximum bracket nesting outside string literals. A cheap guard so hostile input never reaches the
// recursive parser with a depth that could exhaust the stack.
std::size_t json_depth(const std::string& s) {
    std::size_t depth = 0;
    std::size_t max_depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : s) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '[' || c == '{')
            max_depth = std::max(max_depth, ++depth);
        else if ((c == ']' || c == '}') && depth > 0)
            --depth;
    }
    return max_depth;
}

enum class ReadStatus { Line, TooLong, Eof };

// Reads one line without ever buffering more than max_bytes (the rest of an oversized line is discarded).
ReadStatus read_bounded_line(std::istream& in, std::size_t max_bytes, std::string& out,
                             std::size_t& consumed) {
    out.clear();
    consumed = 0;
    auto* sb = in.rdbuf();
    if (sb == nullptr)
        return ReadStatus::Eof;
    bool any = false;
    bool too_long = false;
    for (;;) {
        const int c = sb->sbumpc();
        if (c == std::char_traits<char>::eof())
            break;
        any = true;
        ++consumed;
        if (c == '\n')
            break;
        if (too_long)
            continue;
        if (out.size() >= max_bytes) {
            too_long = true;
            out.clear();
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    if (!any)
        return ReadStatus::Eof;
    if (too_long)
        return ReadStatus::TooLong;
    if (!out.empty() && out.back() == '\r')
        out.pop_back();
    return ReadStatus::Line;
}

bool is_alnum(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}
bool is_token_char(char c) {
    return is_alnum(c) || c == '_' || c == '-';
}

// Counts consecutive characters from `from` that satisfy `pred`.
template <class Pred>
std::size_t run_length(std::string_view s, std::size_t from, Pred pred) {
    std::size_t n = 0;
    while (from + n < s.size() && pred(s[from + n])) ++n;
    return n;
}

bool contains_secret(std::string_view s) {
    // PEM private keys.
    for (std::size_t p = s.find("-----BEGIN"); p != std::string_view::npos; p = s.find("-----BEGIN", p + 1)) {
        if (s.substr(p, 64).find("PRIVATE KEY") != std::string_view::npos)
            return true;
    }
    struct Shape {
        std::string_view prefix;
        std::size_t min_tail;
        bool upper_digits_only;
    };
    static constexpr Shape kShapes[] = {
        {"hf_", 30, false},         {"sk-", 20, false},   {"ghp_", 30, false},
        {"github_pat_", 30, false}, {"xoxb-", 20, false}, {"AKIA", 16, true},
    };
    for (const auto& sh : kShapes) {
        for (std::size_t p = s.find(sh.prefix); p != std::string_view::npos; p = s.find(sh.prefix, p + 1)) {
            if (p > 0 && is_alnum(s[p - 1]))
                continue;  // not at a token start
            const std::size_t from = p + sh.prefix.size();
            const std::size_t n =
                sh.upper_digits_only
                    ? run_length(s, from,
                                 [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); })
                    : run_length(s, from, is_token_char);
            if (n >= sh.min_tail)
                return true;
        }
    }
    return false;
}

// Whitespace-collapsed, ASCII-lower-cased text: two records that differ only in spacing/case are duplicates.
void normalise_into(std::string& out, std::string_view s) {
    std::string seg;  // normalised per segment so leading whitespace never depends on what precedes it
    bool pending_space = false;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c)) {
            pending_space = !seg.empty();
            continue;
        }
        if (pending_space) {
            seg.push_back(' ');
            pending_space = false;
        }
        seg.push_back(static_cast<char>(std::tolower(c)));
    }
    out += seg;
}

using Key = std::pair<std::uint64_t, std::uint64_t>;

// Two independently seeded FNV-1a 64-bit streams: a 128-bit key makes accidental collisions negligible for
// the record counts allowed by DatasetLimits. This is deduplication, not a security hash.
Key dedupe_key(const std::string& text) {
    std::uint64_t h1 = 0xcbf29ce484222325ULL;
    std::uint64_t h2 = 0x84222325cbf29ce4ULL;
    for (const char ch : text) {
        const auto b = static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        h1 = (h1 ^ b) * 0x100000001b3ULL;
        h2 = (h2 ^ (b + 0x9e)) * 0x100000001b3ULL;
    }
    return {h1, h2};
}

struct Rejection {
    IssueKind kind;
    std::string detail;
};

bool role_valid(const std::string& r) {
    return r == "system" || r == "user" || r == "assistant" || r == "tool";
}

bool tool_name_valid(const std::string& n) {
    if (n.empty() || n.size() > 128)
        return false;
    return std::all_of(n.begin(), n.end(),
                       [](char c) { return is_alnum(c) || c == '_' || c == '.' || c == '-'; });
}

}  // namespace

std::string_view to_string(IssueKind k) noexcept {
    switch (k) {
        case IssueKind::MalformedJson:
            return "malformed_json";
        case IssueKind::LineTooLong:
            return "line_too_long";
        case IssueKind::SchemaViolation:
            return "schema_violation";
        case IssueKind::LicenceMissing:
            return "licence_missing";
        case IssueKind::LicenceNotAllowed:
            return "licence_not_allowed";
        case IssueKind::ForbiddenProvenance:
            return "forbidden_provenance";
        case IssueKind::LimitExceeded:
            return "limit_exceeded";
        case IssueKind::SecretDetected:
            return "secret_detected";
        case IssueKind::Duplicate:
            return "duplicate";
    }
    return "unknown";
}

namespace {

// Validates one parsed record. On success fills `canonical_licence` and `normalised` (the dedupe text).
std::optional<Rejection> check_record(const json& rec, const DatasetLimits& lim, const DatasetPolicy& pol,
                                      std::string& canonical_licence, std::string& normalised) {
    auto schema = [](std::string d) { return Rejection{IssueKind::SchemaViolation, std::move(d)}; };
    if (!rec.is_object())
        return schema("record is not a JSON object");

    const auto string_field = [&](const char* name, std::size_t max_len) -> const std::string* {
        const auto it = rec.find(name);
        if (it == rec.end() || !it->is_string())
            return nullptr;
        const auto& s = it->get_ref<const std::string&>();
        return (s.empty() || s.size() > max_len) ? nullptr : &s;
    };

    if (!string_field("id", 256))
        return schema("`id` must be a non-empty string (max 256 bytes)");

    // Licence first: a record we may not use is rejected regardless of any other defect.
    const auto lic_it = rec.find("licence");
    if (lic_it == rec.end() || !lic_it->is_string() || trim(lic_it->get_ref<const std::string&>()).empty())
        return Rejection{IssueKind::LicenceMissing, "`licence` is required (SPDX identifier)"};
    const std::string lic = lower(trim(lic_it->get_ref<const std::string&>()));
    canonical_licence.clear();
    for (const auto& allowed : pol.allowed_licences) {
        if (lower(allowed) == lic) {
            canonical_licence = allowed;
            break;
        }
    }
    if (canonical_licence.empty())
        return Rejection{
            IssueKind::LicenceNotAllowed,
            "licence '" + trim(lic_it->get_ref<const std::string&>()) + "' is not on the allow-list"};

    const std::string* source = string_field("source", 2048);
    if (!source)
        return schema("`source` must be a non-empty string (url or dataset name, max 2048 bytes)");
    const std::string* generator = string_field("generator", 256);
    if (!generator)
        return schema(
            "`generator` is required: \"human\" or the id of the model that wrote the assistant turns");

    const std::string gen_l = lower(*generator);
    const std::string src_l = lower(*source);
    for (const auto& bad : pol.forbidden_generator_substrings) {
        const std::string b = lower(bad);
        if (b.empty())
            continue;
        if (gen_l.find(b) != std::string::npos || src_l.find(b) != std::string::npos)
            return Rejection{IssueKind::ForbiddenProvenance, "provenance names a forbidden model family ('" +
                                                                 b + "'): distilled output is not allowed"};
    }

    const auto msgs_it = rec.find("messages");
    if (msgs_it == rec.end() || !msgs_it->is_array() || msgs_it->empty())
        return schema("`messages` must be a non-empty array");
    if (msgs_it->size() > lim.max_messages)
        return Rejection{IssueKind::LimitExceeded,
                         "more than " + std::to_string(lim.max_messages) + " messages"};

    if (const auto tools_it = rec.find("tools"); tools_it != rec.end()) {
        if (!tools_it->is_array() ||
            !std::all_of(tools_it->begin(), tools_it->end(), [](const json& t) { return t.is_object(); }))
            return schema("`tools` must be an array of objects");
    }

    bool has_user = false;
    std::string last_role;
    std::string all_text;  // for the secret scan
    normalised.clear();
    for (const auto& m : *msgs_it) {
        if (!m.is_object())
            return schema("a message is not an object");
        const auto role_it = m.find("role");
        if (role_it == m.end() || !role_it->is_string() ||
            !role_valid(role_it->get_ref<const std::string&>()))
            return schema("message `role` must be system, user, assistant or tool");
        const std::string& role = role_it->get_ref<const std::string&>();

        const auto tc_it = m.find("tool_calls");
        const bool has_tool_calls = tc_it != m.end() && !tc_it->is_null();
        if (has_tool_calls && role != "assistant")
            return schema("only assistant messages may carry `tool_calls`");

        std::string content;
        const auto c_it = m.find("content");
        if (c_it != m.end() && c_it->is_string()) {
            content = c_it->get_ref<const std::string&>();
        } else if (c_it == m.end() || c_it->is_null()) {
            if (!has_tool_calls)
                return schema("message `content` is required (null is allowed only with `tool_calls`)");
        } else {
            return schema("message `content` must be a string");
        }
        if (content.size() > lim.max_message_bytes)
            return Rejection{IssueKind::LimitExceeded,
                             "a message exceeds " + std::to_string(lim.max_message_bytes) + " bytes"};
        if (!has_tool_calls && is_blank(content))
            return schema("message `content` is empty");

        if (role == "tool") {
            const auto id_it = m.find("tool_call_id");
            if (id_it == m.end() || !id_it->is_string() || id_it->get_ref<const std::string&>().empty())
                return schema("a tool message needs `tool_call_id`");
        }

        std::string call_text;
        if (has_tool_calls) {
            if (!tc_it->is_array() || tc_it->empty())
                return schema("`tool_calls` must be a non-empty array");
            for (const auto& call : *tc_it) {
                const auto fn_it = call.is_object() ? call.find("function") : call.end();
                if (!call.is_object() || fn_it == call.end() || !fn_it->is_object())
                    return schema("a tool call needs a `function` object");
                const auto n_it = fn_it->find("name");
                if (n_it == fn_it->end() || !n_it->is_string() ||
                    !tool_name_valid(n_it->get_ref<const std::string&>()))
                    return schema("tool call `function.name` is missing or not a plain identifier");
                const auto a_it = fn_it->find("arguments");
                if (a_it == fn_it->end())
                    return schema("tool call `function.arguments` is missing");
                if (a_it->is_object()) {
                    call_text += a_it->dump();
                } else if (a_it->is_string()) {
                    const std::string& as = a_it->get_ref<const std::string&>();
                    if (as.size() > lim.max_message_bytes)
                        return Rejection{IssueKind::LimitExceeded,
                                         "tool call arguments exceed the message cap"};
                    if (pol.require_tool_call_json) {
                        if (json_depth(as) > kMaxJsonDepth)
                            return schema("tool call arguments nest too deeply");
                        const json parsed = json::parse(as, nullptr, false);
                        if (parsed.is_discarded() || !parsed.is_object())
                            return schema("tool call arguments are not a valid JSON object");
                    }
                    call_text += as;
                } else {
                    return schema("tool call `function.arguments` must be an object or a JSON string");
                }
                call_text += n_it->get_ref<const std::string&>();
            }
        }

        has_user = has_user || role == "user";
        last_role = role;
        all_text += content;
        all_text += '\n';
        all_text += call_text;
        all_text += '\n';
        normalised += role;
        normalised += '\x1f';
        normalise_into(normalised, content);
        normalised += '\x1f';
        normalise_into(normalised, call_text);
        normalised += '\x1e';
    }
    if (!has_user)
        return schema("conversation has no user message");
    if (last_role != "assistant")
        return schema("conversation must end with an assistant message (the training target)");

    if (pol.reject_secrets && contains_secret(all_text))
        return Rejection{IssueKind::SecretDetected, "record contains something that looks like a credential"};
    return std::nullopt;
}

}  // namespace

ValidationReport DatasetValidator::validate(std::istream& in) const {
    ValidationReport rep;
    std::set<Key> seen;
    std::string line;
    std::size_t record_count = 0;

    const auto reject = [&](std::size_t line_no, IssueKind kind, std::string detail) {
        ++rep.rejected;
        if (rep.issues.size() < limits_.max_issues)
            rep.issues.push_back({line_no, kind, std::move(detail)});
    };

    for (;;) {
        std::size_t consumed = 0;
        const ReadStatus st = read_bounded_line(in, limits_.max_line_bytes, line, consumed);
        if (st == ReadStatus::Eof)
            break;
        ++rep.lines_read;
        rep.bytes_read += consumed;
        const std::size_t line_no = rep.lines_read;

        if (st == ReadStatus::TooLong) {
            reject(line_no, IssueKind::LineTooLong,
                   "line exceeds " + std::to_string(limits_.max_line_bytes) + " bytes and was skipped");
        } else if (!is_blank(line)) {
            ++record_count;
            if (record_count > limits_.max_records) {
                rep.truncated_by_limits = true;
                reject(
                    line_no, IssueKind::LimitExceeded,
                    "more than " + std::to_string(limits_.max_records) + " records; the rest was not read");
                break;
            }
            if (json_depth(line) > kMaxJsonDepth) {
                reject(line_no, IssueKind::MalformedJson,
                       "JSON nests deeper than " + std::to_string(kMaxJsonDepth));
            } else {
                const json rec = json::parse(line, nullptr, false);
                if (rec.is_discarded()) {
                    reject(line_no, IssueKind::MalformedJson, "line is not valid JSON");
                } else {
                    std::string licence;
                    std::string normalised;
                    if (auto bad = check_record(rec, limits_, policy_, licence, normalised)) {
                        reject(line_no, bad->kind, std::move(bad->detail));
                    } else if (!seen.insert(dedupe_key(normalised)).second) {
                        ++rep.duplicates;
                        reject(line_no, IssueKind::Duplicate, "same conversation as an earlier record");
                    } else {
                        ++rep.accepted;
                        ++rep.accepted_by_licence[licence];
                        rep.accepted_lines.push_back(line_no);
                    }
                }
            }
        }

        if (rep.bytes_read > limits_.max_total_bytes) {
            rep.truncated_by_limits = true;
            reject(
                line_no, IssueKind::LimitExceeded,
                "input exceeds " + std::to_string(limits_.max_total_bytes) + " bytes; the rest was not read");
            break;
        }
    }
    return rep;
}

ValidationReport DatasetValidator::validate_string(const std::string& jsonl) const {
    std::istringstream in(jsonl);
    return validate(in);
}

}  // namespace hcs::train
