#include "hcs/train/chat_template.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace hcs::train {

namespace {

constexpr std::size_t kMaxJsonDepth = 128;

// String member or "" when absent / not a string (json::value() would throw on a wrongly typed member).
std::string str_member(const OJson& o, const char* key) {
    if (!o.is_object())
        return {};
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Iterative (no recursion): the deepest nesting level of `v`; scalars are depth 0.
std::size_t json_depth(const OJson& v) {
    std::size_t deepest = 0;
    std::vector<std::pair<const OJson*, std::size_t>> stack{{&v, 0}};
    while (!stack.empty()) {
        const auto [node, depth] = stack.back();
        stack.pop_back();
        deepest = std::max(deepest, depth);
        if (deepest > kMaxJsonDepth)
            return deepest;
        if (node->is_structured())
            for (const auto& child : *node) stack.emplace_back(&child, depth + 1);
    }
    return deepest;
}

Error unsupported(const std::string& m) {
    return Error(ErrorCode::Unsupported, "chat template: " + m);
}

void dump_into(std::string& out, const OJson& v, std::size_t depth = 0) {
    if (depth > kMaxJsonDepth) {
        out += "null";  // render_lfm3 rejects such input up front; this only protects direct template_json
                        // callers
        return;
    }
    switch (v.type()) {
        case OJson::value_t::object: {
            out += '{';
            bool first = true;
            for (const auto& [k, val] : v.items()) {
                if (!first)
                    out += ", ";
                first = false;
                out += OJson(k).dump();
                out += ": ";
                dump_into(out, val, depth + 1);
            }
            out += '}';
            break;
        }
        case OJson::value_t::array: {
            out += '[';
            bool first = true;
            for (const auto& val : v) {
                if (!first)
                    out += ", ";
                first = false;
                dump_into(out, val, depth + 1);
            }
            out += ']';
            break;
        }
        default:
            out += v.dump();  // scalars: strings are escaped like Python's json.dumps(ensure_ascii=False)
    }
}

// `render_value` of the template: strings raw, booleans true/false, null, containers via tojson, numbers
// as-is.
std::string render_value(const OJson& v) {
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    if (v.is_null())
        return "null";
    if (v.is_object() || v.is_array())
        return template_json(v);
    return v.dump();
}

bool has_think_tag(const std::string& s) {
    return s.find("<think>") != std::string::npos || s.find("</think>") != std::string::npos;
}

Result<std::string> render_tool_calls(const OJson& calls) {
    if (!calls.is_array())
        return Error(ErrorCode::InvalidArgument, "tool_calls must be an array");
    std::string out;
    bool first = true;
    for (const auto& call : calls) {
        if (!call.is_object())
            return Error(ErrorCode::InvalidArgument, "a tool call is not an object");
        const OJson* fn = &call;
        if (const auto it = call.find("function"); it != call.end())
            fn = &*it;
        if (!fn->is_object() || !fn->contains("name") || !(*fn)["name"].is_string())
            return Error(ErrorCode::InvalidArgument, "tool call has no function name");
        OJson args = fn->contains("arguments") ? (*fn)["arguments"] : OJson::object();
        if (args.is_string()) {
            const std::string s = args.get<std::string>();
            if (s.empty() || s == "{}" || s == "null") {
                args = OJson::object();
            } else {
                args = OJson::parse(s, nullptr, false);
                if (args.is_discarded())
                    return Error(ErrorCode::InvalidArgument, "tool call arguments are not valid JSON");
            }
        }
        if (args.is_null())
            args = OJson::object();
        if (!args.is_object())
            return Error(ErrorCode::InvalidArgument, "tool call arguments must be a JSON object");
        if (!first)
            out += "\n\n";
        first = false;
        out += "<tool_call>\n<function=" + (*fn)["name"].get<std::string>() + ">\n";
        for (const auto& [k, v] : args.items())
            out += "<parameter=" + k + ">\n" + render_value(v) + "\n</parameter>\n";
        out += "</function>\n</tool_call>";
    }
    return out;
}

// Content of an assistant turn as the template emits it (before the closing <|im_end|>).
Result<std::string> assistant_body(const OJson& m) {
    std::string content;
    if (const auto it = m.find("content"); it != m.end() && !it->is_null()) {
        if (!it->is_string())
            return unsupported("only string message content is supported");
        content = it->get<std::string>();
    }
    if (has_think_tag(content))
        return unsupported("thinking tags inside assistant content are not supported");
    std::string body = "<think></think>" + content;
    if (const auto it = m.find("tool_calls"); it != m.end() && !it->is_null()) {
        auto tc = render_tool_calls(*it);
        if (!tc)
            return tc.error();
        body += tc.value();
    }
    return body;
}

}  // namespace

std::string template_json(const OJson& v) {
    std::string out;
    dump_into(out, v);
    return out;
}

Result<std::string> render_lfm3(const OJson& messages_in, const OJson& tools, bool add_generation_prompt) {
    if (!messages_in.is_array())
        return Error(ErrorCode::InvalidArgument, "messages must be an array");
    if (json_depth(messages_in) > kMaxJsonDepth || json_depth(tools) > kMaxJsonDepth)
        return Error(ErrorCode::InvalidArgument,
                     "chat template: JSON nesting is deeper than the supported limit");
    std::string out(kLfm3Bos);
    std::size_t first = 0;
    std::string system_prompt;
    if (!messages_in.empty() && messages_in[0].is_object() &&
        str_member(messages_in[0], "role") == "system") {
        const auto& c = messages_in[0].contains("content") ? messages_in[0]["content"] : OJson();
        if (c.is_string())
            system_prompt = c.get<std::string>();
        else if (!c.is_null())
            return unsupported("only string system content is supported");
        first = 1;
    }
    if (tools.is_array() && !tools.empty()) {
        std::string lines;
        for (const auto& t : tools) {
            if (!t.is_object())
                return Error(ErrorCode::InvalidArgument, "a tool definition is not an object");
            const OJson& fn = t.contains("function") ? t["function"] : t;
            if (!fn.is_object())
                return Error(ErrorCode::InvalidArgument, "tool.function is not an object");
            std::string line = "{";
            bool f = true;
            for (const auto& [k, v] : fn.items()) {
                if (k == "defer_loading")
                    continue;
                if (!f)
                    line += ", ";
                f = false;
                line += OJson(k).dump() + ": " + template_json(v);
            }
            line += "}";
            if (!lines.empty())
                lines += "\n";
            lines += line;
        }
        if (!lines.empty())
            system_prompt +=
                std::string(system_prompt.empty() ? "" : "\n\n") + "<tools>\n" + lines + "\n</tools>";
    }
    if (!system_prompt.empty())
        out += "<|im_start|>system\n" + system_prompt + "<|im_end|>\n";

    std::string prev_tool_id;
    for (std::size_t i = first; i < messages_in.size(); ++i) {
        const OJson& m = messages_in[i];
        if (!m.is_object() || !m.contains("role") || !m["role"].is_string())
            return Error(ErrorCode::InvalidArgument, "a message has no role");
        const std::string role = m["role"].get<std::string>();
        out += "<|im_start|>" + role + "\n";
        if (role == "assistant") {
            auto body = assistant_body(m);
            if (!body)
                return body.error();
            out += body.value() + "<|im_end|>\n";
            prev_tool_id.clear();
        } else if (role == "tool") {
            std::string id = str_member(m, "tool_call_id");
            if (!id.empty() && id == prev_tool_id)
                return unsupported(
                    "consecutive tool messages with one tool_call_id are merged by the template");
            prev_tool_id = id;
            if (!m.contains("content") || !m["content"].is_string())
                return unsupported("tool message content must be a string");
            out += "<tool_response>\n" + m["content"].get<std::string>() + "\n</tool_response><|im_end|>\n";
        } else {
            prev_tool_id.clear();
            if (m.contains("content") && !m["content"].is_null()) {
                if (!m["content"].is_string())
                    return unsupported("only string message content is supported");
                out += m["content"].get<std::string>();
            }
            out += "<|im_end|>\n";
        }
    }
    if (add_generation_prompt)
        out += "<|im_start|>assistant\n<think></think>";
    return out;
}

Result<std::vector<SftExample>> to_sft_examples(const OJson& record) {
    if (!record.is_object() || !record.contains("messages") || !record["messages"].is_array())
        return Error(ErrorCode::InvalidArgument, "record has no messages array");
    // Checked before anything is copied: copying (and recursively dumping) a hostile deep value would
    // overflow the stack.
    if (json_depth(record) > kMaxJsonDepth)
        return Error(ErrorCode::InvalidArgument,
                     "chat template: JSON nesting is deeper than the supported limit");
    const OJson& msgs = record["messages"];
    const OJson tools = record.contains("tools") ? record["tools"] : OJson::array();
    const std::string id = str_member(record, "id");
    std::vector<SftExample> out;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (!msgs[i].is_object() || str_member(msgs[i], "role") != "assistant")
            continue;
        OJson before = OJson::array();
        for (std::size_t j = 0; j < i; ++j) before.push_back(msgs[j]);
        auto prompt = render_lfm3(before, tools, true);
        if (!prompt)
            return prompt.error();
        auto body = assistant_body(msgs[i]);
        if (!body)
            return body.error();
        SftExample ex;
        ex.id = id + "#" + std::to_string(i);
        ex.prompt = prompt.value();
        // The prompt already carries "<think></think>"; the completion is what follows it.
        ex.completion = body.value().substr(std::string("<think></think>").size()) + "<|im_end|>";
        // Self-consistency: the full render up to turn i must equal prompt + completion + "\n".
        OJson upto = before;
        upto.push_back(msgs[i]);
        auto full = render_lfm3(upto, tools, false);
        if (!full)
            return full.error();
        if (full.value() != ex.prompt + ex.completion + "\n")
            return Error(ErrorCode::Internal,
                         "chat template: prompt/completion split disagrees with the full render");
        out.push_back(std::move(ex));
    }
    if (out.empty())
        return Error(ErrorCode::InvalidArgument, "record has no assistant turn to learn from");
    return out;
}

}  // namespace hcs::train
