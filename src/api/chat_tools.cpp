#include "hcs/api/chat_tools.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace hcs::api::chat_tools {
namespace {

constexpr std::string_view kCallOpen = "<tool_call>";
constexpr std::string_view kCallClose = "</tool_call>";

bool valid_tool_name(const std::string& n) {
    if (n.empty() || n.size() > 64)
        return false;
    return std::all_of(n.begin(), n.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
    });
}

// Python json.dumps(..., ensure_ascii=False) layout (", " and ": "): the layout the chat template's `tojson`
// produced for the model's training data.
void py_dumps(const json& j, std::string& out) {
    switch (j.type()) {
        case json::value_t::object: {
            out += '{';
            bool first = true;
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (!first)
                    out += ", ";
                first = false;
                out += json(it.key()).dump(-1, ' ', false, json::error_handler_t::replace);
                out += ": ";
                py_dumps(it.value(), out);
            }
            out += '}';
            break;
        }
        case json::value_t::array: {
            out += '[';
            bool first = true;
            for (const auto& e : j) {
                if (!first)
                    out += ", ";
                first = false;
                py_dumps(e, out);
            }
            out += ']';
            break;
        }
        default:
            out += j.dump(-1, ' ', false, json::error_handler_t::replace);
    }
}

std::string py_dumps(const json& j) {
    std::string s;
    py_dumps(j, s);
    return s;
}

std::string render_value(const json& v) {
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    if (v.is_null())
        return "null";
    return py_dumps(v);
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::string rtrim(std::string_view s) {
    std::size_t e = s.size();
    while (e > 0 && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(0, e));
}

// Parameter value from XML text, typed by the tool's JSON schema where it is known.
json coerce(const std::string& raw, const ToolDef* tool, const std::string& pname) {
    std::string type;
    if (tool && tool->parameters.is_object()) {
        const auto pit = tool->parameters.find("properties");
        if (pit != tool->parameters.end() && pit->is_object()) {
            const auto it = pit->find(pname);
            if (it != pit->end() && it->is_object()) {
                const auto t = it->find("type");
                if (t != it->end() && t->is_string())
                    type = t->get<std::string>();
            }
        }
    }
    if (type == "string")
        return raw;
    const std::string t = trim(raw);
    json v = json::parse(t, nullptr, false);
    if (!v.is_discarded())
        return v;
    return raw;
}

const ToolDef* find_tool(const std::vector<ToolDef>& tools, const std::string& name) {
    for (const auto& t : tools)
        if (t.name == name)
            return &t;
    return nullptr;
}

// One `<tool_call>` body (between the tags). Returns false if it is neither the XML nor the JSON form.
bool parse_call_body(std::string_view body, const std::vector<ToolDef>& tools, ToolCall& out) {
    const std::string b = trim(body);
    if (b.empty())
        return false;
    if (b.front() == '{') {  // Hermes: {"name": "...", "arguments": {...}}
        json j = json::parse(b, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("name") || !j["name"].is_string())
            return false;
        out.name = j["name"].get<std::string>();
        json args = j.contains("arguments") ? j["arguments"] : json::object();
        if (args.is_string()) {
            json parsed = json::parse(args.get<std::string>(), nullptr, false);
            args = parsed.is_object() ? parsed : json::object();
        }
        out.arguments = args.is_object() ? args : json::object();
        return !out.name.empty();
    }
    const std::size_t f = b.find("<function=");
    if (f == std::string::npos)
        return false;
    const std::size_t fe = b.find('>', f);
    if (fe == std::string::npos)
        return false;
    out.name = trim(std::string_view(b).substr(f + 10, fe - (f + 10)));
    if (out.name.empty())
        return false;
    const ToolDef* tool = find_tool(tools, out.name);
    out.arguments = json::object();
    std::size_t pos = fe + 1;
    for (;;) {
        const std::size_t p = b.find("<parameter=", pos);
        if (p == std::string::npos)
            break;
        const std::size_t pe = b.find('>', p);
        if (pe == std::string::npos)
            break;
        const std::string pname = trim(std::string_view(b).substr(p + 11, pe - (p + 11)));
        const std::size_t close = b.find("</parameter>", pe + 1);
        std::string val;
        if (close == std::string::npos) {
            // Unterminated value: take up to the next tag boundary.
            const std::size_t nxt = b.find("</function>", pe + 1);
            val = b.substr(pe + 1, nxt == std::string::npos ? std::string::npos : nxt - (pe + 1));
            pos = nxt == std::string::npos ? b.size() : nxt;
        } else {
            val = b.substr(pe + 1, close - (pe + 1));
            pos = close + 12;
        }
        // The template puts the value on its own line: drop exactly that one leading/trailing newline.
        if (!val.empty() && val.front() == '\n')
            val.erase(0, 1);
        if (!val.empty() && val.back() == '\n')
            val.pop_back();
        if (!pname.empty())
            out.arguments[pname] = coerce(val, tool, pname);
    }
    return true;
}

}  // namespace

Result<ToolsRequest> parse_tools_request(const json& body) {
    for (const char* k : {"functions", "function_call"}) {
        if (body.contains(k))
            return make_error(ErrorCode::Unsupported,
                              std::string("'") + k + "' (legacy functions) is not supported; use 'tools'");
    }
    ToolsRequest req;
    const bool has_tools = body.contains("tools") && !body["tools"].is_null();
    if (body.contains("tool_choice") && !body["tool_choice"].is_null() && !has_tools)
        return make_error(ErrorCode::InvalidArgument, "'tool_choice' needs 'tools'");
    if (!has_tools)
        return req;
    const json& arr = body["tools"];
    if (!arr.is_array())
        return make_error(ErrorCode::InvalidArgument, "'tools' must be an array");
    if (arr.size() > kMaxTools)
        return make_error(ErrorCode::InvalidArgument,
                          "'tools' has more than " + std::to_string(kMaxTools) + " entries");
    if (arr.dump().size() > kMaxToolsJsonBytes)
        return make_error(
            ErrorCode::InvalidArgument,
            "'tools' exceeds the " + std::to_string(kMaxToolsJsonBytes / 1024) + " KiB definition limit");
    std::set<std::string> seen;
    for (const auto& t : arr) {
        if (!t.is_object() || !t.contains("type") || !t["type"].is_string() ||
            t["type"].get<std::string>() != "function" || !t.contains("function") ||
            !t["function"].is_object())
            return make_error(ErrorCode::InvalidArgument,
                              "each tool must be {\"type\":\"function\",\"function\":{...}}");
        const json& f = t["function"];
        if (!f.contains("name") || !f["name"].is_string() || !valid_tool_name(f["name"].get<std::string>()))
            return make_error(ErrorCode::InvalidArgument,
                              "tool function.name must be 1-64 characters of [A-Za-z0-9_.-]");
        ToolDef d;
        d.name = f["name"].get<std::string>();
        if (!seen.insert(d.name).second)
            return make_error(ErrorCode::InvalidArgument, "duplicate tool name '" + d.name + "'");
        if (f.contains("description")) {
            if (!f["description"].is_string())
                return make_error(ErrorCode::InvalidArgument, "tool function.description must be a string");
            d.description = f["description"].get<std::string>();
        }
        if (f.contains("parameters") && !f["parameters"].is_null()) {
            if (!f["parameters"].is_object())
                return make_error(ErrorCode::InvalidArgument, "tool function.parameters must be an object");
            d.parameters = f["parameters"];
        }
        req.tools.push_back(std::move(d));
    }
    req.mode = ToolMode::Auto;
    if (body.contains("tool_choice") && !body["tool_choice"].is_null()) {
        const json& c = body["tool_choice"];
        if (c.is_string() && c.get<std::string>() == "auto") {
            req.mode = ToolMode::Auto;
        } else if (c.is_string() && c.get<std::string>() == "none") {
            req.mode = ToolMode::None;
        } else if (c.is_string() || c.is_object()) {
            return make_error(ErrorCode::Unsupported,
                              "tool_choice 'required' or a named function is not supported (nothing "
                              "constrains the sampler to emit a call); use \"auto\" or \"none\"");
        } else {
            return make_error(ErrorCode::InvalidArgument, "'tool_choice' must be a string or an object");
        }
    }
    if (req.tools.empty())
        req.mode = ToolMode::Off;
    return req;
}

std::string render_tools_block(const std::vector<ToolDef>& tools) {
    std::string out = "<tools>\n";
    for (std::size_t i = 0; i < tools.size(); ++i) {
        // Key order name, description, parameters as in the chat template (nlohmann objects are sorted, so
        // the top level is assembled by hand).
        if (i)
            out += '\n';
        out += "{\"name\": " + py_dumps(json(tools[i].name));
        if (!tools[i].description.empty())
            out += ", \"description\": " + py_dumps(json(tools[i].description));
        out += ", \"parameters\": " + py_dumps(tools[i].parameters) + "}";
    }
    out += "\n</tools>";
    return out;
}

Result<std::string> render_assistant_tool_calls(const json& tool_calls) {
    if (!tool_calls.is_array())
        return make_error(ErrorCode::InvalidArgument, "'tool_calls' must be an array");
    if (tool_calls.size() > kMaxToolCallsPerMessage)
        return make_error(ErrorCode::InvalidArgument, "too many tool_calls in one message");
    std::string out;
    bool first = true;
    for (const auto& tc : tool_calls) {
        const json* fn = &tc;
        if (tc.is_object() && tc.contains("function") && tc["function"].is_object())
            fn = &tc["function"];
        if (!fn->is_object() || !fn->contains("name") || !(*fn)["name"].is_string() ||
            !valid_tool_name((*fn)["name"].get<std::string>()))
            return make_error(ErrorCode::InvalidArgument, "tool_calls[].function.name is missing or invalid");
        json args = fn->contains("arguments") ? (*fn)["arguments"] : json::object();
        if (args.is_string()) {
            const std::string s = trim(args.get<std::string>());
            if (s.empty() || s == "null") {
                args = json::object();
            } else {
                json parsed = json::parse(s, nullptr, false);
                if (parsed.is_discarded() || !parsed.is_object())
                    return make_error(
                        ErrorCode::InvalidArgument,
                        "tool_calls[].function.arguments must be a JSON object (string-encoded)");
                args = std::move(parsed);
            }
        }
        if (!args.is_object())
            return make_error(ErrorCode::InvalidArgument,
                              "tool_calls[].function.arguments must be a JSON object");
        if (!first)
            out += "\n\n";
        first = false;
        out += "<tool_call>\n<function=" + (*fn)["name"].get<std::string>() + ">\n";
        for (auto it = args.begin(); it != args.end(); ++it)
            out += "<parameter=" + it.key() + ">\n" + render_value(it.value()) + "\n</parameter>\n";
        out += "</function>\n</tool_call>";
    }
    return out;
}

std::string render_tool_response(const std::string& content) {
    if (content.find("<tool_response>") != std::string::npos)
        return content;
    return "<tool_response>\n" + content + "\n</tool_response>";
}

ParsedOutput parse_output(std::string_view text, const std::vector<ToolDef>& tools, std::uint64_t seq) {
    ParsedOutput out;
    std::string content;
    std::size_t pos = 0;
    std::size_t n = 0;
    while (pos < text.size()) {
        const std::size_t open = text.find(kCallOpen, pos);
        if (open == std::string_view::npos) {
            content.append(text.substr(pos));
            break;
        }
        content.append(text.substr(pos, open - pos));
        const std::size_t body_begin = open + kCallOpen.size();
        std::size_t close = text.find(kCallClose, body_begin);
        std::string_view body;
        std::size_t next;
        if (close == std::string_view::npos) {
            body = text.substr(body_begin);  // truncated: accept only if the function element is complete
            next = text.size();
            if (body.find("</function>") == std::string_view::npos && trim(body).rfind('{', 0) != 0) {
                content.append(text.substr(open));
                break;
            }
        } else {
            body = text.substr(body_begin, close - body_begin);
            next = close + kCallClose.size();
        }
        ToolCall c;
        if (parse_call_body(body, tools, c) && out.calls.size() < kMaxToolCallsPerMessage) {
            c.id = "call_" + std::to_string(seq) + "_" + std::to_string(n++);
            out.calls.push_back(std::move(c));
        } else {
            content.append(text.substr(open, next - open));  // unparseable: keep it visible, never drop text
        }
        pos = next;
    }
    out.content = trim(content);
    return out;
}

std::string StreamSplitter::feed(std::string_view piece) {
    if (in_call_) {
        call_.append(piece);
        return {};
    }
    pending_.append(piece);
    const std::size_t idx = pending_.find(kCallOpen);
    if (idx != std::string::npos) {
        std::string emit = rtrim(std::string_view(pending_).substr(0, idx));
        call_ = pending_.substr(idx);
        pending_.clear();
        in_call_ = true;
        emitted_ += emit;
        return emit;
    }
    // Hold back a possible tag prefix and any trailing whitespace (it may precede a call).
    std::size_t hold = 0;
    for (std::size_t k = std::min(kCallOpen.size() - 1, pending_.size()); k > 0; --k) {
        if (pending_.compare(pending_.size() - k, k, kCallOpen, 0, k) == 0) {
            hold = k;
            break;
        }
    }
    std::size_t safe = pending_.size() - hold;
    while (safe > 0 && std::isspace(static_cast<unsigned char>(pending_[safe - 1]))) --safe;
    std::string emit = pending_.substr(0, safe);
    pending_.erase(0, safe);
    emitted_ += emit;
    return emit;
}

ParsedOutput StreamSplitter::finish() {
    ParsedOutput out;
    if (!in_call_) {
        out.content = rtrim(pending_);
        pending_.clear();
        return out;
    }
    ParsedOutput p = parse_output(call_, tools_, seq_);
    if (p.calls.empty()) {  // nothing usable: hand the held text back as content
        out.content = call_;
        return out;
    }
    out.calls = std::move(p.calls);
    out.content = std::move(p.content);
    return out;
}

}  // namespace hcs::api::chat_tools
