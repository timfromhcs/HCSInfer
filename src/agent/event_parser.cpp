#include "hcs/agent/event_parser.hpp"

#include "hcs/agent/agent_redact.hpp"

namespace hcs::agent {

using nlohmann::json;

namespace {

// Hostile input may carry the right key with the wrong type; json::value() would throw on that.
std::string str_of(const json& o, const char* key) {
    if (!o.is_object())
        return {};
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool bool_of(const json& o, const char* key, bool fallback) {
    const auto it = o.find(key);
    return it != o.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::int64_t int_of(const json& o, const char* key, std::int64_t fallback) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_number())
        return fallback;
    if (it->is_number_integer())
        return it->get<std::int64_t>();
    const double d = it->get<double>();
    return d >= 0 && d < 9e15 ? static_cast<std::int64_t>(d) : fallback;
}

std::string join_text(const json& content, std::size_t cap, bool& truncated) {
    std::string out;
    if (content.is_string()) {
        out = content.get<std::string>();
    } else if (content.is_array()) {
        for (const auto& part : content) {
            if (part.is_object() && str_of(part, "type") == "text" && part.contains("text") &&
                part["text"].is_string())
                out += part["text"].get<std::string>();
        }
    }
    return cap_utf8(out, cap, truncated);
}

}  // namespace

std::vector<ParsedEvent> parse_agent_line(std::string_view line, std::size_t cap) {
    std::vector<ParsedEvent> out;
    if (line.empty() || line.front() != '{')
        return out;
    json j = json::parse(line.begin(), line.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("type") || !j["type"].is_string())
        return out;
    const std::string type = j["type"].get<std::string>();  // is_string checked above
    ParsedEvent ev;
    if (type == "agent_start") {
        ev.kind = "agent_start";
        out.push_back(std::move(ev));
    } else if (type == "tool_execution_start") {
        ev.kind = "tool_call";
        ev.tool = str_of(j, "toolName");
        const json args = j.contains("args") ? j["args"] : json::object();
        std::string payload;
        if (args.is_object()) {
            if (args.contains("code") && args["code"].is_string())
                payload = args["code"].get<std::string>();
            else if (args.contains("command") && args["command"].is_string())
                payload = args["command"].get<std::string>();
        }
        bool trunc = false;
        ev.payload = cap_utf8(payload, cap, trunc);
        ev.data = {{"id", str_of(j, "toolCallId")},
                   {"tool", ev.tool},
                   {"bytes", payload.size()},
                   {"text", ev.payload},
                   {"truncated", trunc}};
        out.push_back(std::move(ev));
    } else if (type == "tool_execution_end") {
        ev.kind = "tool_result";
        bool trunc = false;
        const std::string text =
            j.contains("result") && j["result"].is_object() && j["result"].contains("content")
                ? join_text(j["result"]["content"], cap, trunc)
                : std::string();
        std::int64_t dur = -1;
        if (j.contains("result") && j["result"].is_object() && j["result"].contains("details") &&
            j["result"]["details"].is_object() && j["result"]["details"].contains("durationMs") &&
            j["result"]["details"]["durationMs"].is_number())
            dur = int_of(j["result"]["details"], "durationMs", -1);
        ev.tool = str_of(j, "toolName");
        ev.data = {{"id", str_of(j, "toolCallId")},
                   {"tool", ev.tool},
                   {"ok", !bool_of(j, "isError", false)},
                   {"bytes", text.size()},
                   {"duration_ms", dur},
                   {"text", text},
                   {"truncated", trunc}};
        out.push_back(std::move(ev));
    } else if (type == "turn_end") {
        if (!j.contains("message") || !j["message"].is_object())
            return out;
        const json& m = j["message"];
        if (str_of(m, "role") != "assistant")
            return out;
        bool trunc = false;
        const std::string text = m.contains("content") ? join_text(m["content"], cap, trunc) : std::string();
        if (!text.empty()) {
            ParsedEvent t;
            t.kind = "assistant_text";
            t.data = {{"text", text}, {"truncated", trunc}};
            out.push_back(std::move(t));
        }
        if (m.contains("usage") && m["usage"].is_object()) {
            ParsedEvent u;
            u.kind = "usage";
            u.data = {{"input_tokens", int_of(m["usage"], "input", 0)},
                      {"output_tokens", int_of(m["usage"], "output", 0)},
                      {"stop_reason", str_of(m, "stopReason")}};
            out.push_back(std::move(u));
        }
    } else if (type == "agent_end") {
        ev.kind = "agent_end";
        out.push_back(std::move(ev));
    }
    return out;
}

}  // namespace hcs::agent
