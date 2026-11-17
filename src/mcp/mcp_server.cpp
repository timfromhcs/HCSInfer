#include "hcs/mcp/mcp_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <list>

#include "hcs/api/json_schema.hpp"
#include "hcs/fs_util.hpp"

namespace hcs::mcp {
namespace {

using json = nlohmann::json;
constexpr const char* kMeta = "_meta";
constexpr const char* kMetaVersion = "io.modelcontextprotocol/protocolVersion";
constexpr const char* kMetaCaps = "io.modelcontextprotocol/clientCapabilities";
constexpr const char* kMetaServerInfo = "io.modelcontextprotocol/serverInfo";

bool method_is(const json& m, const char* name) {
    return m.is_object() && m.contains("method") && m["method"].is_string() && m["method"].get<std::string>() == name;
}
bool jsonrpc_ok(const json& m) {
    return m.is_object() && m.contains("jsonrpc") && m["jsonrpc"].is_string() && m["jsonrpc"].get<std::string>() == "2.0";
}

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternal = -32603;
constexpr int kUnsupportedVersion = -32022;

std::string dump_safe(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

json str_schema(std::size_t max_len, const char* desc) {
    return {{"type", "string"}, {"maxLength", max_len}, {"description", desc}};
}

json message_array_schema() {
    return {{"type", "array"},
            {"minItems", 1},
            {"maxItems", 128},
            {"description", "Chat messages (text only)."},
            {"items",
             {{"type", "object"},
              {"required", {"role", "content"}},
              {"additionalProperties", false},
              {"properties",
               {{"role", {{"type", "string"}, {"enum", {"system", "user", "assistant"}}}},
                {"content", {{"type", "string"}, {"maxLength", 262144}}}}}}}};
}

json tool(const std::string& name, const std::string& title, const std::string& desc, json props,
          std::vector<std::string> required, bool read_only, bool destructive = false) {
    json schema = {{"type", "object"}, {"properties", std::move(props)}, {"additionalProperties", false}};
    if (!required.empty()) schema["required"] = std::move(required);
    return {{"name", name},
            {"title", title},
            {"description", desc},
            {"inputSchema", schema},
            {"annotations",
             {{"readOnlyHint", read_only}, {"destructiveHint", destructive}, {"openWorldHint", false}}}};
}

bool ids_equal_key(const json& id, std::string& key) {
    if (id.is_string() || id.is_number_integer()) {
        key = dump_safe(id);
        return true;
    }
    return false;
}

}  // namespace

json tool_list(std::uint32_t cap) {
    const json id = str_schema(64, "Model id from hcs.models.list");
    const json model_opt = str_schema(64, "Model id to use (loads it if needed). Omit to use the loaded model.");
    json tools = json::array();
    tools.push_back(tool("hcs.models.list", "List models", "List registered models and their lifecycle state.", json::object(), {}, true));
    tools.push_back(tool("hcs.models.inspect", "Inspect model", "Show the manifest and state of one model (no file paths).",
                         {{"id", id}}, {"id"}, true));
    tools.push_back(tool("hcs.models.load", "Load model",
                         "Load a registered model into the text engine (file must be inside an approved root). "
                         "Unloads the previously loaded model first.",
                         {{"id", id}}, {"id"}, false));
    tools.push_back(tool("hcs.models.unload", "Unload model", "Unload the currently loaded model (cancels running generation).",
                         json::object(), {}, false, true));
    tools.push_back(tool(
        "hcs.chat.generate", "Generate text",
        "Generate a chat completion with the loaded (or given) model. Provide either 'prompt' (one user message) or 'messages'.",
        {{"model", model_opt},
         {"prompt", str_schema(262144, "A single user message.")},
         {"messages", message_array_schema()},
         {"max_tokens", {{"type", "integer"}, {"minimum", 1}, {"maximum", cap}}},
         {"temperature", {{"type", "number"}, {"minimum", 0}, {"maximum", 2}}},
         {"seed", {{"type", "integer"}, {"minimum", 0}}}},
        {}, false));
    tools.push_back(tool("hcs.vision.analyze", "Analyze image",
                         "Answer a question about an image (png/jpeg/gif/bmp file inside an approved root) with the loaded "
                         "(or given) multimodal model. The model must have its mmproj projector next to it; otherwise the "
                         "call reports 'unsupported'.",
                         {{"model", model_opt}, {"prompt", str_schema(65536, "Question about the image.")},
                          {"image_path", str_schema(1024, "Image file inside an approved root.")},
                          {"max_tokens", {{"type", "integer"}, {"minimum", 1}, {"maximum", cap}}},
                          {"temperature", {{"type", "number"}, {"minimum", 0}, {"maximum", 2}}},
                          {"seed", {{"type", "integer"}, {"minimum", 0}}}},
                         {"prompt", "image_path"}, true));
    tools.push_back(tool("hcs.image.generate", "Generate image",
                         "Text-to-image. Currently reports 'unsupported': no image engine adapter yet.",
                         {{"model", model_opt}, {"prompt", str_schema(65536, "Image prompt.")},
                          {"width", {{"type", "integer"}, {"minimum", 64}, {"maximum", 2048}}},
                          {"height", {{"type", "integer"}, {"minimum", 64}, {"maximum", 2048}}},
                          {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 150}}},
                          {"seed", {{"type", "integer"}, {"minimum", 0}}}},
                         {"prompt"}, false));
    tools.push_back(tool("hcs.image.edit", "Edit image",
                         "Image-to-image edit. Currently reports 'unsupported': no image engine adapter yet.",
                         {{"model", model_opt}, {"prompt", str_schema(65536, "Edit instruction.")},
                          {"input_path", str_schema(1024, "Input image inside an approved root.")}},
                         {"prompt", "input_path"}, false));
    tools.push_back(tool("hcs.runtime.status", "Runtime status", "Engine, loaded model, memory budget and (optionally) device report.",
                         {{"include_devices", {{"type", "boolean"}, {"description", "Include the hardware/device report (may take a moment)."}}}},
                         {}, true));
    tools.push_back(tool("hcs.jobs.status", "Job status", "State of a job returned by another tool or the HTTP API.",
                         {{"job_id", str_schema(24, "e.g. job-12")}}, {"job_id"}, true));
    tools.push_back(tool("hcs.jobs.cancel", "Cancel job", "Cancel a queued or running job.",
                         {{"job_id", str_schema(24, "e.g. job-12")}}, {"job_id"}, false, true));
    return tools;
}

McpServer::McpServer(api::ControlPlane& cp, McpOptions options, Logger& logger)
    : cp_(cp), opt_(std::move(options)), log_(logger) {
    tools_ = tool_list(cp_.options().max_tokens_cap);
    for (const auto& t : tools_) schemas_[t["name"].get<std::string>()] = t["inputSchema"];
}

McpServer::~McpServer() = default;

json McpServer::rpc_error(const json& id, int code, const std::string& msg, json data) const {
    json e = {{"code", code}, {"message", msg}};
    if (!data.is_null()) e["data"] = std::move(data);
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(e)}};
}

json McpServer::ok_result(const json& id, json result, bool modern) const {
    if (modern) {
        result["resultType"] = "complete";
        result[kMeta][kMetaServerInfo] = {{"name", opt_.server_name}, {"version", opt_.server_version}};
    }
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json McpServer::tool_ok(json structured) const {
    return {{"content", json::array({json{{"type", "text"}, {"text", dump_safe(structured)}}})},
            {"structuredContent", std::move(structured)},
            {"isError", false}};
}

json McpServer::tool_err(const std::string& msg, const std::string& code) const {
    return {{"content", json::array({json{{"type", "text"}, {"text", code + ": " + msg}}})},
            {"structuredContent", {{"error", {{"code", code}, {"message", msg}}}}},
            {"isError", true}};
}

json McpServer::tool_err(const Error& e) const { return tool_err(e.message, std::string(to_string(e.code))); }

void McpServer::cancel_request(const json& request_id) {
    std::string key;
    if (!ids_equal_key(request_id, key)) return;  // malformed: ignore (spec)
    std::uint64_t job = 0;
    {
        std::lock_guard lk(mu_);
        auto it = cancelled_.find(key);
        if (it == cancelled_.end()) return;  // unknown/finished: ignore
        it->second = true;
        if (auto j = running_jobs_.find(key); j != running_jobs_.end()) job = j->second;
    }
    if (job != 0) (void)cp_.cancel_job(JobId{job});
}

// Waits for a chat job, honouring MCP cancellation and the hard timeout.
json McpServer::wait_chat(const api::Submitted& sub, const std::string& req_key) {
    using clock = std::chrono::steady_clock;
    {
        std::lock_guard lk(mu_);
        running_jobs_[req_key] = sub.id.value;
    }
    auto is_cancelled = [&] {
        std::lock_guard lk(mu_);
        auto it = cancelled_.find(req_key);
        return it != cancelled_.end() && it->second;
    };
    if (is_cancelled()) (void)cp_.cancel_job(sub.id);
    const auto deadline = clock::now() + opt_.call_timeout;
    std::string text;
    bool done = false, timed_out = false;
    while (!done) {
        JobEvent ev;
        const auto slice = std::min(deadline, clock::now() + std::chrono::milliseconds(100));
        switch (sub.channel->pop(ev, slice)) {
            case api::JobChannel::Pop::Event:
                if (ev.kind == JobEventKind::Output) text += ev.text;
                break;
            case api::JobChannel::Pop::Done: done = true; break;
            case api::JobChannel::Pop::Timeout:
                if (clock::now() >= deadline) {
                    (void)cp_.cancel_job(sub.id);
                    timed_out = true;
                    done = true;
                }
                break;
        }
    }
    (void)cp_.wait_job(sub.id, std::chrono::milliseconds(2000));
    {
        std::lock_guard lk(mu_);
        running_jobs_.erase(req_key);
    }
    if (timed_out) return tool_err("tool call timed out", "timeout");
    auto st = cp_.job_status(sub.id);
    if (!st) return tool_err(st.error());
    if (st.value().state != JobState::Succeeded) {
        if (st.value().error) return tool_err(*st.value().error);
        return tool_err("job was cancelled", "cancelled");
    }
    json out = {{"text", text}, {"job_id", to_string(sub.id)}};
    if (auto r = cp_.chat_result(sub.id)) {
        out["finish_reason"] = r.value().finish_reason;
        out["usage"] = {{"prompt_tokens", r.value().prompt_tokens},
                        {"completion_tokens", r.value().completion_tokens}};
    }
    if (auto lm = cp_.loaded_model()) out["model"] = *lm;
    return tool_ok(std::move(out));
}

json McpServer::call_tool(const std::string& name, const json& args, const std::string& req_key) {
    using api::ControlPlane;
    if (name == "hcs.models.list") {
        json arr = json::array();
        for (const auto& v : cp_.models()) arr.push_back(cp_.model_json(v));
        return tool_ok({{"models", arr}});
    }
    if (name == "hcs.models.inspect") {
        auto v = cp_.model(args["id"].get<std::string>());
        if (!v) return tool_err(v.error());
        return tool_ok(cp_.model_json(v.value()));
    }
    if (name == "hcs.models.load") {
        const std::string id = args["id"].get<std::string>();
        auto job = cp_.submit_load(id);
        if (!job) return tool_err(job.error());
        {
            std::lock_guard lk(mu_);
            running_jobs_[req_key] = job.value().value;
        }
        const auto deadline = std::chrono::steady_clock::now() + opt_.call_timeout;
        bool timed_out = false;
        while (!cp_.wait_job(job.value(), std::chrono::milliseconds(100))) {
            bool cancelled;
            {
                std::lock_guard lk(mu_);
                auto it = cancelled_.find(req_key);
                cancelled = it != cancelled_.end() && it->second;
            }
            if (cancelled) (void)cp_.cancel_job(job.value());
            if (std::chrono::steady_clock::now() >= deadline) {
                (void)cp_.cancel_job(job.value());
                timed_out = true;
                break;
            }
        }
        {
            std::lock_guard lk(mu_);
            running_jobs_.erase(req_key);
        }
        if (timed_out) return tool_err("model load timed out", "timeout");
        auto st = cp_.job_status(job.value());
        if (!st) return tool_err(st.error());
        if (st.value().state != JobState::Succeeded)
            return st.value().error ? tool_err(*st.value().error) : tool_err("load was cancelled", "cancelled");
        return tool_ok({{"id", id}, {"state", "loaded"}});
    }
    if (name == "hcs.models.unload") {
        if (auto r = cp_.unload_model(); !r) return tool_err(r.error());
        return tool_ok({{"state", "unloaded"}});
    }
    if (name == "hcs.chat.generate") {
        api::ChatParams p;
        p.model = args.value("model", "");
        const bool has_prompt = args.contains("prompt"), has_msgs = args.contains("messages");
        if (has_prompt == has_msgs) return tool_err("provide exactly one of 'prompt' or 'messages'", "invalid_argument");
        if (has_prompt) p.messages.push_back({"user", args["prompt"].get<std::string>()});
        else
            for (const auto& m : args["messages"]) p.messages.push_back({m["role"].get<std::string>(), m["content"].get<std::string>()});
        if (args.contains("max_tokens")) p.max_tokens = args["max_tokens"].get<std::uint32_t>();
        if (args.contains("temperature")) p.temperature = args["temperature"].get<float>();
        if (args.contains("seed")) p.seed = args["seed"].get<std::uint64_t>();
        auto sub = cp_.submit_chat(p);
        if (!sub) return tool_err(sub.error());
        return wait_chat(sub.value(), req_key);
    }
    if (name == "hcs.vision.analyze") {
        // The path rule is enforced before anything is read: nothing outside an approved root is probed.
        auto resolved = cp_.guard().resolve(args["image_path"].get<std::string>());
        if (!resolved) return tool_err(resolved.error());
        auto bytes = read_file_limited(resolved.value(), opt_.max_image_bytes);
        if (!bytes) return tool_err(bytes.error());
        api::ChatParams p;
        p.model = args.value("model", "");
        api::ChatMessage m;
        m.role = "user";
        m.content = args["prompt"].get<std::string>();
        m.images.push_back(ImageInput{std::vector<std::uint8_t>(bytes.value().begin(), bytes.value().end())});
        p.messages.push_back(std::move(m));
        p.temperature = 0.2f;
        if (args.contains("max_tokens")) p.max_tokens = args["max_tokens"].get<std::uint32_t>();
        if (args.contains("temperature")) p.temperature = args["temperature"].get<float>();
        if (args.contains("seed")) p.seed = args["seed"].get<std::uint64_t>();
        auto sub = cp_.submit_chat(p);
        if (!sub) return tool_err(sub.error());
        return wait_chat(sub.value(), req_key);
    }
    if (name == "hcs.vision.analyze" || name == "hcs.image.edit") {
        const char* key = name == "hcs.vision.analyze" ? "image_path" : "input_path";
        // Even for an unsupported tool the path rule is enforced first: nothing outside an approved
        // root is ever probed.
        auto resolved = cp_.guard().resolve(args[key].get<std::string>());
        if (!resolved) return tool_err(resolved.error());
        return tool_err(name == "hcs.vision.analyze"
                            ? "vision is not available: no multimodal engine adapter is implemented yet"
                            : "image editing is not available: no image engine adapter is implemented yet",
                        "unsupported");
    }
    if (name == "hcs.image.generate")
        return tool_err("image generation is not available: no image engine adapter is implemented yet", "unsupported");
    if (name == "hcs.runtime.status") {
        json j = {{"runtime", cp_.runtime_json()}, {"memory", cp_.memory_json()}};
        if (args.value("include_devices", false)) j["devices"] = cp_.devices_json();
        return tool_ok(std::move(j));
    }
    if (name == "hcs.jobs.status" || name == "hcs.jobs.cancel") {
        auto id = ControlPlane::parse_job_id(args["job_id"].get<std::string>());
        if (!id) return tool_err("malformed job id", "invalid_argument");
        if (name == "hcs.jobs.cancel") {
            if (auto r = cp_.cancel_job(*id); !r) return tool_err(r.error());
        }
        auto st = cp_.job_status(*id);
        if (!st) return tool_err(st.error());
        return tool_ok(cp_.job_json(st.value()));
    }
    return tool_err("unknown tool", "not_found");
}

json McpServer::handle_message(const json& msg, const std::function<bool()>&, const std::string& req_key_in) {
    if (!msg.is_object() || !jsonrpc_ok(msg) || !msg.contains("method") || !msg["method"].is_string())
        return rpc_error(nullptr, kInvalidRequest, "not a valid JSON-RPC 2.0 request or notification");
    const std::string method = msg["method"].get<std::string>();
    const bool is_request = msg.contains("id");
    const json id = is_request ? msg["id"] : json(nullptr);
    if (is_request && !(id.is_string() || id.is_number_integer()))
        return rpc_error(nullptr, kInvalidRequest, "id must be a string or integer");
    const json params = msg.contains("params") ? msg["params"] : json::object();
    if (msg.contains("params") && !params.is_object()) {
        return is_request ? rpc_error(id, kInvalidParams, "params must be an object") : json(nullptr);
    }

    if (!is_request) {  // notifications: never answered
        if (method == "notifications/cancelled" && params.contains("requestId")) cancel_request(params["requestId"]);
        else if (method == "notifications/initialized") {
            std::lock_guard lk(mu_);
            if (legacy_session_) legacy_initialized_ = true;
        }
        return nullptr;
    }

    const json supported = json::array({kModernVersion, kLegacyVersion});
    if (method == "initialize") {
        if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string())
            return rpc_error(id, kInvalidParams, "initialize requires protocolVersion");
        {
            std::lock_guard lk(mu_);
            legacy_session_ = true;
            legacy_initialized_ = false;
        }
        // Legacy handshake: only 2025-11-25 is served this way; the client decides whether to continue.
        json r = {{"protocolVersion", kLegacyVersion},
                  {"capabilities", {{"tools", {{"listChanged", false}}}}},
                  {"serverInfo", {{"name", opt_.server_name}, {"version", opt_.server_version}}},
                  {"instructions", "Local HCSInfer control plane. Tools operate on registered models inside approved roots."}};
        return ok_result(id, std::move(r), false);
    }

    bool legacy;
    {
        std::lock_guard lk(mu_);
        legacy = legacy_session_;
    }
    if (method == "server/discover") {
        if (params.contains(kMeta) && params[kMeta].is_object() && params[kMeta].contains(kMetaVersion) &&
            params[kMeta][kMetaVersion].is_string()) {
            const std::string v = params[kMeta][kMetaVersion].get<std::string>();
            if (v != kModernVersion && v != kLegacyVersion)
                return rpc_error(id, kUnsupportedVersion, "Unsupported protocol version",
                                 {{"supported", supported}, {"requested", v}});
        }
        json r = {{"supportedVersions", supported},
                  {"capabilities", {{"tools", {{"listChanged", false}}}}},
                  {"serverInfo", {{"name", opt_.server_name}, {"version", opt_.server_version}}},
                  {"instructions", "Local HCSInfer control plane. Tools operate on registered models inside approved roots."}};
        return ok_result(id, std::move(r), true);
    }

    if (legacy) {
        bool inited;
        {
            std::lock_guard lk(mu_);
            inited = legacy_initialized_;
        }
        if (method == "ping") return ok_result(id, json::object(), false);
        if (!inited) return rpc_error(id, kInvalidRequest, "session not initialized: send notifications/initialized first");
    } else {
        if (!params.contains(kMeta) || !params[kMeta].is_object() || !params[kMeta].contains(kMetaVersion) ||
            !params[kMeta][kMetaVersion].is_string())
            return rpc_error(id, kInvalidParams,
                             std::string("missing params._meta[\"") + kMetaVersion +
                                 "\"]; this server speaks MCP " + kModernVersion + " (stateless). Legacy clients must send initialize first.");
        const std::string v = params[kMeta][kMetaVersion].get<std::string>();
        if (v != kModernVersion) {
            if (v == kLegacyVersion)
                return rpc_error(id, kInvalidParams, "protocol " + std::string(kLegacyVersion) + " requires the initialize handshake first");
            return rpc_error(id, kUnsupportedVersion, "Unsupported protocol version", {{"supported", supported}, {"requested", v}});
        }
        if (!params[kMeta].contains(kMetaCaps) || !params[kMeta][kMetaCaps].is_object())
            return rpc_error(id, kInvalidParams, std::string("missing params._meta[\"") + kMetaCaps + "\"]");
    }
    const bool modern = !legacy;

    if (method == "tools/list") {
        json r = {{"tools", tools_}};
        if (modern) {
            r["ttlMs"] = 3600000;
            r["cacheScope"] = "public";
        }
        return ok_result(id, std::move(r), modern);
    }
    if (method == "tools/call") {
        if (!params.contains("name") || !params["name"].is_string())
            return rpc_error(id, kInvalidParams, "tools/call requires a string 'name'");
        const std::string name = params["name"].get<std::string>();
        auto sit = schemas_.find(name);
        if (sit == schemas_.end()) return rpc_error(id, kInvalidParams, "unknown tool: " + name.substr(0, 128));
        json args = params.contains("arguments") ? params["arguments"] : json::object();
        if (!args.is_object()) return rpc_error(id, kInvalidParams, "'arguments' must be an object");
        // Invalid arguments are tool execution errors, not protocol errors (spec: Tools).
        if (auto v = api::validate_json(sit->second, args); !v)
            return ok_result(id, tool_err(v.error().message, "invalid_argument"), modern);
        json res;
        try {
            res = call_tool(name, args, req_key_in);
        } catch (const std::exception& e) {
            log_.error("mcp", "tool threw", {{"tool", name}, {"what", e.what()}});
            res = tool_err("internal error", "internal");
        }
        return ok_result(id, std::move(res), modern);
    }
    return rpc_error(id, kMethodNotFound, "method not found: " + method.substr(0, 128));
}

std::string McpServer::handle_line(const std::string& line) {
    json msg = json::parse(line, nullptr, false);
    if (msg.is_discarded()) return dump_safe(rpc_error(nullptr, kParseError, "parse error"));
    std::string key;
    const bool has_key = msg.is_object() && msg.contains("id") && ids_equal_key(msg["id"], key);
    if (has_key) {
        std::lock_guard lk(mu_);
        cancelled_[key] = false;
    }
    json resp = handle_message(msg, {}, key);
    bool cancelled = false;
    if (has_key) {
        std::lock_guard lk(mu_);
        cancelled = cancelled_[key];
        cancelled_.erase(key);
    }
    if (resp.is_null() || cancelled) return {};
    return dump_safe(resp);
}

struct McpServer::CallSlot {};

void McpServer::run(std::istream& in, std::ostream& out) {
    struct Worker {
        std::thread t;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::list<Worker> workers;
    std::mutex out_mu;
    auto write = [&](const json& j) {
        std::lock_guard lk(out_mu);
        out << dump_safe(j) << '\n';
        out.flush();
    };
    auto reap = [&] {
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->done->load()) {
                it->t.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
    };

    std::string line;
    bool overflow = false;
    char c;
    auto process = [&](const std::string& text, bool too_long) {
        if (too_long) {
            write(rpc_error(nullptr, kInvalidRequest, "message exceeds " + std::to_string(opt_.max_line_bytes) + " bytes"));
            return;
        }
        if (text.find_first_not_of(" \t\r") == std::string::npos) return;
        json msg = json::parse(text, nullptr, false);
        if (msg.is_discarded()) {
            write(rpc_error(nullptr, kParseError, "parse error"));
            return;
        }
        if (msg.is_array()) {
            write(rpc_error(nullptr, kInvalidRequest, "JSON-RPC batches are not supported"));
            return;
        }
        std::string key;
        const bool is_call = msg.is_object() && msg.contains("id") && method_is(msg, "tools/call") &&
                             ids_equal_key(msg["id"], key);
        if (!is_call) {
            std::string k2;
            const bool has_key = msg.is_object() && msg.contains("id") && ids_equal_key(msg["id"], k2);
            json resp;
            try {
                resp = handle_message(msg, {}, k2);
            } catch (const std::exception& e) {
                log_.error("mcp", "handler threw", {{"what", e.what()}});
                resp = rpc_error(has_key ? msg["id"] : json(nullptr), kInternal, "internal error");
            }
            if (!resp.is_null()) write(resp);
            return;
        }
        // tools/call: worker thread so the reader can keep consuming notifications/cancelled.
        reap();
        {
            std::lock_guard lk(mu_);
            if (active_calls_ >= opt_.max_concurrent_calls) {
                write(ok_result(msg["id"], tool_err("too many concurrent tool calls", "busy"), !legacy_session_));
                return;
            }
            ++active_calls_;
            cancelled_[key] = false;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers.push_back({std::thread([this, msg, key, done, &write] {
                               json resp;
                               try {
                                   resp = handle_message(msg, {}, key);
                               } catch (const std::exception& e) {
                                   log_.error("mcp", "call threw", {{"what", e.what()}});
                                   resp = rpc_error(msg["id"], kInternal, "internal error");
                               }
                               bool cancelled;
                               {
                                   std::lock_guard lk(mu_);
                                   cancelled = cancelled_[key];
                                   cancelled_.erase(key);
                                   --active_calls_;
                               }
                               // Spec: no response for a cancelled request.
                               if (!cancelled && !resp.is_null()) write(resp);
                               done->store(true);
                           }),
                           done});
    };

    while (in.get(c)) {
        if (c == '\n') {
            process(line, overflow);
            line.clear();
            overflow = false;
        } else if (!overflow) {
            line.push_back(c);
            if (line.size() > opt_.max_line_bytes) {
                overflow = true;
                line.clear();
            }
        }
    }
    if (!line.empty() || overflow) process(line, overflow);

    // EOF = shutdown signal. In-flight calls get a short grace period, then are cancelled.
    const auto grace = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < grace) {
        reap();
        if (workers.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!workers.empty()) {
        std::vector<std::string> keys;
        {
            std::lock_guard lk(mu_);
            for (auto& [k, v] : cancelled_) keys.push_back(k);
        }
        for (const auto& k : keys) {
            try {
                cancel_request(json::parse(k));
            } catch (...) {}
        }
        for (auto& w : workers) w.t.join();
    }
}

}  // namespace hcs::mcp
