#include "hcs/api/http_api.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <exception>
#include <random>
#include <sstream>

#include "hcs/api/base64.hpp"
#include "hcs/api/chat_tools.hpp"
#include "hcs/api/data_uri.hpp"
#include "hcs/api/image_probe.hpp"
#include "hcs/api/json_guard.hpp"
#include "hcs/api/json_schema.hpp"
#include "hcs/api/redact.hpp"
#ifdef HCS_HAVE_GUI
#include "hcs/gui/gui_assets.hpp"
#endif

namespace hcs::api {
namespace {

using json = nlohmann::json;

// Model output is not guaranteed to be valid UTF-8 (a stray continuation byte, a flushed partial sequence):
// never let serialisation throw and turn a successful generation into a 500 (M10 audit C4).
std::string dump_safe(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ct_equal(std::string_view a, std::string_view b) {
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

int http_status_for(ErrorCode c) {
    switch (c) {
        case ErrorCode::Ok:
            return 200;
        case ErrorCode::InvalidArgument:
            return 400;
        case ErrorCode::ModelError:
            return 422;
        case ErrorCode::BackendError:
            return 502;
        case ErrorCode::OutOfMemory:
            return 503;
        case ErrorCode::Cancelled:
            return 409;
        case ErrorCode::NotFound:
            return 404;
        case ErrorCode::IoError:
            return 500;
        case ErrorCode::SecurityViolation:
            return 403;
        case ErrorCode::Unsupported:
            return 501;
        case ErrorCode::InvalidState:
        case ErrorCode::AlreadyExists:
            return 409;  // both are "the request conflicts with the current state"
        case ErrorCode::Busy:
            return 429;
        case ErrorCode::Timeout:
            return 504;
        case ErrorCode::Internal:
            return 500;
    }
    return 500;
}

std::string type_for(int status) {
    if (status == 401)
        return "authentication_error";
    if (status == 403)
        return "permission_error";
    if (status == 404)
        return "not_found_error";
    if (status == 429)
        return "rate_limit_error";
    if (status >= 500)
        return "server_error";
    return "invalid_request_error";
}

std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < p.size()) {
        if (p[i] == '/') {
            ++i;
            continue;
        }
        std::size_t j = p.find('/', i);
        if (j == std::string::npos)
            j = p.size();
        out.push_back(p.substr(i, j - i));
        i = j;
    }
    return out;
}

// Parses "host[:port]" / "[v6][:port]" into host (lower-case, brackets kept for v6).
std::string host_part(const std::string& hp) {
    std::string h = lower(hp);
    if (!h.empty() && h[0] == '[') {
        const auto e = h.find(']');
        return e == std::string::npos ? h : h.substr(0, e + 1);
    }
    const auto c = h.find(':');
    return c == std::string::npos ? h : h.substr(0, c);
}

bool is_ipv4_literal(const std::string& h) {
    int dots = 0;
    if (h.empty())
        return false;
    for (char c : h) {
        if (c == '.')
            ++dots;
        else if (c < '0' || c > '9')
            return false;
    }
    return dots == 3 && h.front() != '.' && h.back() != '.' && h.find("..") == std::string::npos;
}

bool is_ipv6_literal(const std::string& h) {
    return h.size() >= 3 && h.front() == '[' && h.back() == ']';
}

}  // namespace

std::string generate_token() {
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i) {
        const std::uint32_t v = rd();
        for (int b = 0; b < 8; ++b) out.push_back(hex[(v >> (4 * b)) & 0xF]);
    }
    return out;  // 64 hex chars = 256 bits
}

// OpenAI tool_calls array; `delta` adds the per-item "index" that streaming chunks carry.
static json tool_calls_json(const std::vector<chat_tools::ToolCall>& calls, bool delta) {
    json arr = json::array();
    std::size_t i = 0;
    for (const auto& c : calls) {
        json item = {{"id", c.id},
                     {"type", "function"},
                     {"function",
                      {{"name", c.name},
                       {"arguments", c.arguments.dump(-1, ' ', false, json::error_handler_t::replace)}}}};
        if (delta)
            item["index"] = i;
        arr.push_back(std::move(item));
        ++i;
    }
    return arr;
}

json chat_completions_schema(std::uint32_t cap) {
    static const char* kSchema = R"JSON({
  "type": "object",
  "required": ["messages"],
  "properties": {
    "model": {"type": "string", "maxLength": 64},
    "messages": {
      "type": "array", "minItems": 1, "maxItems": 128,
      "items": {
        "type": "object",
        "required": ["role"],
        "properties": {
          "role": {"type": "string", "enum": ["system", "user", "assistant", "tool"]},
          "content": {"anyOf": [
            {"type": "null"},
            {"type": "string"},
            {"type": "array", "maxItems": 64,
             "items": {"type": "object", "required": ["type"], "properties": {"type": {"type": "string"}}}}
          ]},
          "tool_calls": {"type": "array", "maxItems": 16, "items": {"type": "object"}},
          "tool_call_id": {"type": "string", "maxLength": 128},
          "name": {"type": "string", "maxLength": 128}
        }
      }
    },
    "tools": {"type": ["array", "null"], "maxItems": 64, "items": {"type": "object"}},
    "tool_choice": {"type": ["string", "object", "null"]},
    "max_tokens": {"type": "integer", "minimum": 1},
    "max_completion_tokens": {"type": "integer", "minimum": 1},
    "temperature": {"type": "number", "minimum": 0, "maximum": 2},
    "seed": {"type": "integer", "minimum": 0},
    "stream": {"type": "boolean"},
    "n": {"type": "integer", "enum": [1]}
  }
})JSON";
    json s = json::parse(kSchema);
    s["properties"]["max_tokens"]["maximum"] = cap;
    s["properties"]["max_completion_tokens"]["maximum"] = cap;
    return s;
}

struct ApiHandler::Inflight {
    explicit Inflight(std::atomic<std::size_t>& c) : counter(c) {}
    ~Inflight() { counter.fetch_sub(1); }
    std::atomic<std::size_t>& counter;
};

ApiHandler::ApiHandler(ControlPlane& cp, ApiOptions options, Logger& logger)
    : cp_(cp), opt_(std::move(options)), log_(logger), port_(opt_.port) {
    std::random_device rd;
    rng_state_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    chat_schema_ = chat_completions_schema(cp_.options().max_tokens_cap);
}

void ApiHandler::set_port(int port) {
    port_.store(port);
}

std::string ApiHandler::new_request_id() {
    std::lock_guard lk(rng_mutex_);
    // splitmix64: request ids only need uniqueness, not secrecy.
    rng_state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = rng_state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    static const char* hex = "0123456789abcdef";
    std::string s = "req-";
    for (int i = 15; i >= 0; --i) s.push_back(hex[(z >> (4 * i)) & 0xF]);
    return s;
}

bool ApiHandler::host_allowed(const std::string& host_header) const {
    if (host_header.empty() || host_header.size() > 255)
        return false;
    const std::string h = host_part(host_header);
    if (h == "localhost" || h == "127.0.0.1" || h == "[::1]")
        return true;
    if (h == lower(opt_.bind_host) || h == "[" + lower(opt_.bind_host) + "]")
        return true;
    for (const auto& a : opt_.allowed_hosts)
        if (h == lower(a))
            return true;
    // LAN opt-in: IP literals cannot be DNS-rebound, names can, so names stay allow-listed only.
    if (opt_.lan && (is_ipv4_literal(h) || is_ipv6_literal(h)))
        return true;
    return false;
}

bool ApiHandler::origin_allowed(const std::string& origin) const {
    if (origin.empty() || origin.size() > 255)
        return false;
    for (const auto& a : opt_.allowed_origins)
        if (lower(a) == lower(origin))
            return true;
    const std::string o = lower(origin);
    std::string rest;
    if (o.rfind("http://", 0) == 0)
        rest = o.substr(7);
    else if (o.rfind("https://", 0) == 0)
        rest = o.substr(8);
    else
        return false;  // includes "null" and file:// origins
    if (rest.find('/') != std::string::npos)
        return false;
    if (!host_allowed(rest))
        return false;
    // Same port as this server: another local web app is still a cross-origin caller.
    std::string port_text;
    if (!rest.empty() && rest[0] == '[') {
        const auto e = rest.find(']');
        if (e != std::string::npos && e + 1 < rest.size() && rest[e + 1] == ':')
            port_text = rest.substr(e + 2);
    } else if (const auto c = rest.find(':'); c != std::string::npos) {
        port_text = rest.substr(c + 1);
    }
    return port_text == std::to_string(port_.load());
}

HttpResponse ApiHandler::error(int status, const std::string& type, const std::string& code,
                               const std::string& msg, const std::string& rid) const {
    HttpResponse r;
    r.status = status;
    json j = {{"error",
               {{"message", redact_paths(msg)},
                {"type", type},
                {"code", code},
                {"param", nullptr},
                {"request_id", rid}}}};
    r.body = dump_safe(j);
    return r;
}

HttpResponse ApiHandler::from_error(const Error& e, const std::string& rid) const {
    const int st = http_status_for(e.code);
    auto r = error(st, type_for(st), std::string(to_string(e.code)), e.message, rid);
    if (st == 429)
        r.headers.emplace_back("Retry-After", "1");
    return r;
}

HttpResponse ApiHandler::json_ok(const json& j, int status) const {
    HttpResponse r;
    r.status = status;
    r.body = dump_safe(j);
    return r;
}

HttpResponse ApiHandler::handle(const HttpRequest& req) {
    const std::string rid = new_request_id();
    HttpResponse resp;
    try {
        auto get = [&](const char* k) -> std::string {
            auto it = req.headers.find(k);
            return it == req.headers.end() ? std::string() : it->second;
        };
        const bool is_health = req.method == "GET" && req.path == "/hcs/v1/health";
        if (!host_allowed(get("host"))) {
            resp = error(403, "permission_error", "host_not_allowed", "Host header is not allowed", rid);
        } else if (req.headers.count("origin") && !origin_allowed(get("origin"))) {
            resp = error(403, "permission_error", "origin_not_allowed", "Origin is not allowed", rid);
#ifdef HCS_HAVE_GUI
        } else if (auto g = hcs::gui::serve(req.method, req.path)) {
            // Embedded web UI (ADR-009): static assets, no secret inside, reachable only after the
            // Host/Origin checks above and before bearer auth (the page itself carries no token; the user
            // supplies it in the URL fragment, which is never sent to the server). Not counted as an
            // in-flight API request.
            resp.status = g->status;
            resp.content_type = g->content_type;
            resp.headers = g->headers;
            resp.body = std::string(g->body);
#endif
        } else if (!is_health && [&] {
                       const std::string a = get("authorization");
                       if (a.size() < 8 || lower(a.substr(0, 7)) != "bearer ")
                           return true;
                       return opt_.token.empty() || !ct_equal(a.substr(7), opt_.token);
                   }()) {
            resp = error(401, "authentication_error", "unauthorized", "missing or invalid bearer token", rid);
            resp.headers.emplace_back("WWW-Authenticate", "Bearer");
        } else {
            std::shared_ptr<Inflight> guard;
            // Health and job cancellation are never throttled: cancelling is the way out of overload.
            const bool is_cancel = req.method == "POST" && req.path.rfind("/hcs/v1/jobs/", 0) == 0 &&
                                   req.path.size() > 7 &&
                                   req.path.compare(req.path.size() - 7, 7, "/cancel") == 0;
            if (!is_health && !is_cancel) {
                if (inflight_.fetch_add(1) >= opt_.max_inflight) {
                    inflight_.fetch_sub(1);
                    resp = error(429, "rate_limit_error", "too_many_requests", "too many concurrent requests",
                                 rid);
                    resp.headers.emplace_back("Retry-After", "1");
                } else {
                    guard = std::make_shared<Inflight>(inflight_);
                }
            }
            if (is_health || is_cancel || guard)
                resp = dispatch(req, rid, guard);
        }
    } catch (const std::exception& e) {
        log_.error("api", "unhandled exception", {{"rid", rid}, {"what", e.what()}});
        resp = error(500, "server_error", "internal", "internal error", rid);
    }
    resp.headers.emplace_back("X-Request-Id", rid);
    resp.headers.emplace_back("X-Content-Type-Options", "nosniff");
    resp.headers.emplace_back("Cache-Control", "no-store");
    log_.info(
        "api", "request",
        {{"method", req.method}, {"path", req.path}, {"status", std::to_string(resp.status)}, {"rid", rid}});
    return resp;
}

HttpResponse ApiHandler::dispatch(const HttpRequest& req, const std::string& rid,
                                  const std::shared_ptr<Inflight>& guard) {
    const auto seg = split_path(req.path);
    const bool is_get = req.method == "GET";
    const bool is_post = req.method == "POST";
    auto not_allowed = [&](const char* allow) {
        auto r = error(405, "invalid_request_error", "method_not_allowed",
                       "method not allowed for this route", rid);
        r.headers.emplace_back("Allow", allow);
        return r;
    };
    if (req.path.empty() || req.path[0] != '/' || req.path.find("//") != std::string::npos ||
        (req.path.size() > 1 && req.path.back() == '/'))
        return error(404, "not_found_error", "not_found", "no such route", rid);

    // --- OpenAI-compatible ---
    if (req.path == "/v1/models") {
        if (!is_get)
            return not_allowed("GET");
        json data = json::array();
        for (const auto& v : cp_.models()) {
            if (v.manifest.kind == ModelKind::Image)
                continue;
            data.push_back(
                {{"id", v.manifest.id},
                 {"object", "model"},
                 {"created", 0},
                 {"owned_by", "local"},
                 {"hcs",
                  {{"state", std::string(to_string(v.state))}, {"quantization", v.manifest.quantization}}}});
        }
        return json_ok({{"object", "list"}, {"data", data}});
    }
    if (req.path == "/v1/chat/completions") {
        if (!is_post)
            return not_allowed("POST");
        return chat(req, rid, guard);
    }
    if (req.path == "/v1/images/generations" || req.path == "/v1/images/edits") {
        if (!is_post)
            return not_allowed("POST");
        return images(req, rid, guard, req.path == "/v1/images/edits");
    }

    // --- HCS native ---
    if (seg.size() >= 3 && seg[0] == "hcs" && seg[1] == "v1") {
        const std::string& a = seg[2];
        if (seg.size() == 3) {
            if (a == "health") {
                if (!is_get)
                    return not_allowed("GET");
                return json_ok({{"status", "ok"}, {"version", cp_.options().version}});
            }
            if (a == "runtime" || a == "devices" || a == "memory" || a == "models") {
                if (!is_get)
                    return not_allowed("GET");
                if (a == "runtime")
                    return json_ok(cp_.runtime_json());
                if (a == "devices")
                    return json_ok(cp_.devices_json());
                if (a == "memory")
                    return json_ok(cp_.memory_json());
                json arr = json::array();
                for (const auto& v : cp_.models()) arr.push_back(cp_.model_json(v));
                return json_ok({{"models", arr}});
            }
            if (a == "benchmarks") {
                if (!is_post)
                    return not_allowed("POST");
                return error(501, "server_error", "unsupported", "benchmarks are not implemented yet", rid);
            }
        }
        if (a == "models" && seg.size() == 4) {
            if (!is_get)
                return not_allowed("GET");
            auto v = cp_.model(seg[3]);
            if (!v)
                return from_error(v.error(), rid);
            return json_ok(cp_.model_json(v.value()));
        }
        if (a == "models" && seg.size() == 5 && (seg[4] == "load" || seg[4] == "unload")) {
            if (!is_post)
                return not_allowed("POST");
            if (seg[4] == "load") {
                auto id = cp_.submit_load(seg[3]);
                if (!id)
                    return from_error(id.error(), rid);
                json j = {{"job_id", to_string(id.value())},
                          {"status_url", "/hcs/v1/jobs/" + to_string(id.value())}};
                return json_ok(j, 202);
            }
            auto v = cp_.model(seg[3]);
            if (!v)
                return from_error(v.error(), rid);
            const auto lm = cp_.loaded_model();
            if (!lm || *lm != seg[3])
                return error(409, "invalid_request_error", "invalid_state", "that model is not loaded", rid);
            if (auto r = cp_.unload_model(); !r)
                return from_error(r.error(), rid);
            return json_ok({{"id", seg[3]}, {"state", "unloaded"}});
        }
        if (a == "jobs" && (seg.size() == 4 || (seg.size() == 5 && seg[4] == "cancel"))) {
            auto id = ControlPlane::parse_job_id(seg[3]);
            if (!id)
                return error(400, "invalid_request_error", "invalid_argument", "malformed job id", rid);
            if (seg.size() == 4) {
                if (!is_get)
                    return not_allowed("GET");
                auto st = cp_.job_status(*id);
                if (!st)
                    return from_error(st.error(), rid);
                return json_ok(cp_.job_json(st.value()));
            }
            if (!is_post)
                return not_allowed("POST");
            if (auto r = cp_.cancel_job(*id); !r)
                return from_error(r.error(), rid);
            auto st = cp_.job_status(*id);
            if (!st)
                return from_error(st.error(), rid);
            return json_ok(cp_.job_json(st.value()));
        }
    }
    return error(404, "not_found_error", "not_found", "no such route", rid);
}

HttpResponse ApiHandler::chat(const HttpRequest& req, const std::string& rid,
                              const std::shared_ptr<Inflight>& guard) {
    using clock = std::chrono::steady_clock;
    auto ct = req.headers.find("content-type");
    if (ct == req.headers.end() || lower(ct->second).rfind("application/json", 0) != 0)
        return error(415, "invalid_request_error", "unsupported_media_type",
                     "Content-Type must be application/json", rid);
    if (req.body.size() > opt_.max_body_bytes)
        return error(413, "invalid_request_error", "payload_too_large",
                     "request body exceeds the configured limit", rid);
    if (!json_depth_ok(req.body, kMaxJsonDepth))
        return error(400, "invalid_request_error", "invalid_json", "JSON nesting too deep", rid);
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded())
        return error(400, "invalid_request_error", "invalid_json", "malformed JSON body", rid);
    if (auto v = validate_json(chat_schema_, body); !v)
        return error(400, "invalid_request_error", "invalid_request", v.error().message, rid);
    auto tools_req = chat_tools::parse_tools_request(body);
    if (!tools_req) {
        if (tools_req.error().code == ErrorCode::Unsupported)
            return error(501, "server_error", "unsupported", tools_req.error().message, rid);
        return error(400, "invalid_request_error", "invalid_request", tools_req.error().message, rid);
    }
    const chat_tools::ToolsRequest tools = std::move(tools_req.value());
    const bool tools_on = tools.mode == chat_tools::ToolMode::Auto;
    if (body.contains("response_format") &&
        !(body["response_format"].is_object() && body["response_format"].contains("type") &&
          body["response_format"]["type"].is_string() &&
          body["response_format"]["type"].get<std::string>() == "text"))
        return error(501, "server_error", "unsupported",
                     "only response_format {\"type\":\"text\"} is supported", rid);

    ChatParams p;
    p.model = body.value("model", "");
    for (const auto& m : body["messages"]) {
        ChatMessage cm;
        cm.role = m["role"].get<std::string>();
        if (cm.role == "tool" && !tools_on && tools.mode != chat_tools::ToolMode::None)
            return error(400, "invalid_request_error", "invalid_request",
                         "'tool' messages need a 'tools' array in the request", rid);
        if (m.contains("tool_calls") && cm.role != "assistant")
            return error(400, "invalid_request_error", "invalid_request",
                         "'tool_calls' is only allowed on assistant messages", rid);
        const json c = m.contains("content") ? m["content"] : json(nullptr);
        if (c.is_null()) {
            if (cm.role != "assistant" || !m.contains("tool_calls"))
                return error(400, "invalid_request_error", "invalid_request",
                             "message content is required (null is only allowed on an assistant message "
                             "with tool_calls)",
                             rid);
        } else if (c.is_string()) {
            cm.content = c.get<std::string>();
        } else {
            for (const auto& part : c) {
                const std::string t = part["type"].get<std::string>();
                if (t == "text" && part.contains("text") && part["text"].is_string()) {
                    cm.content += part["text"].get<std::string>();
                } else if (t == "image_url") {
                    // OpenAI shape: {"type":"image_url","image_url":{"url":"data:image/png;base64,..."}}
                    // (a bare string is accepted too). Only data URIs: no outbound fetch, no server paths.
                    std::string uri;
                    if (part.contains("image_url") && part["image_url"].is_string()) {
                        uri = part["image_url"].get<std::string>();
                    } else if (part.contains("image_url") && part["image_url"].is_object() &&
                               part["image_url"].contains("url") && part["image_url"]["url"].is_string()) {
                        uri = part["image_url"]["url"].get<std::string>();
                    } else {
                        return error(400, "invalid_request_error", "invalid_request",
                                     "image_url part needs image_url.url (a data URI)", rid);
                    }
                    if (cm.role != "user")
                        return error(400, "invalid_request_error", "invalid_request",
                                     "images are only allowed in user messages", rid);
                    auto img = decode_image_data_uri(uri, cp_.options().max_image_bytes);
                    if (!img)
                        return error(400, "invalid_request_error", "invalid_image", img.error().message, rid);
                    cm.images.push_back(std::move(img.value()));
                } else if (t == "input_image") {
                    return error(400, "invalid_request_error", "invalid_request",
                                 "unsupported content part type 'input_image'; use {\"type\":\"image_url\"} "
                                 "with a data URI",
                                 rid);
                } else {
                    return error(400, "invalid_request_error", "invalid_request",
                                 "unsupported content part type '" + t + "'", rid);
                }
            }
        }
        if (cm.role == "assistant" && m.contains("tool_calls")) {
            auto rendered = chat_tools::render_assistant_tool_calls(m["tool_calls"]);
            if (!rendered)
                return error(400, "invalid_request_error", "invalid_request", rendered.error().message, rid);
            if (!cm.content.empty())
                cm.content += "\n\n";
            cm.content += rendered.value();
        } else if (cm.role == "tool") {
            cm.content = chat_tools::render_tool_response(cm.content);
        }
        p.messages.push_back(std::move(cm));
    }
    if (tools_on) {
        // The tool definitions travel in the system turn (merged into an existing system message).
        const std::string block = chat_tools::render_tools_block(tools.tools);
        if (!p.messages.empty() && p.messages.front().role == "system") {
            auto& sc = p.messages.front().content;
            sc += (sc.empty() ? "" : "\n\n") + block;
        } else {
            ChatMessage sm;
            sm.role = "system";
            sm.content = block;
            p.messages.insert(p.messages.begin(), std::move(sm));
        }
        p.tools = true;
    } else if (tools.mode == chat_tools::ToolMode::None) {
        // tool_choice "none": the model never sees the tools; history tool messages stay renderable.
        p.tools = true;
    }
    if (body.contains("max_tokens"))
        p.max_tokens = body["max_tokens"].get<std::uint32_t>();
    else if (body.contains("max_completion_tokens"))
        p.max_tokens = body["max_completion_tokens"].get<std::uint32_t>();
    if (body.contains("temperature"))
        p.temperature = body["temperature"].get<float>();
    if (body.contains("seed"))
        p.seed = body["seed"].get<std::uint64_t>();
    const bool stream = body.value("stream", false);

    auto sub = cp_.submit_chat(p);
    if (!sub)
        return from_error(sub.error(), rid);
    const JobId job = sub.value().id;
    auto channel = sub.value().channel;
    const auto deadline = clock::now() + opt_.request_timeout;
    // A request whose client is gone must not keep the single engine worker busy (M10 audit D3). The
    // transport's probe is polled in short slices while we wait for output.
    const auto peer_closed = req.peer_closed;
    constexpr auto kPoll = std::chrono::milliseconds(150);
    auto next_probe = clock::now();
    auto peer_gone = [&peer_closed, &next_probe, kPoll] {
        if (!peer_closed)
            return false;
        const auto now = clock::now();
        if (now < next_probe)
            return false;  // one cheap socket probe per poll interval, not one per token
        next_probe = now + kPoll;
        return peer_closed();
    };
    auto abandoned = [&]() {
        (void)cp_.cancel_job(job);
        (void)cp_.wait_job(job, std::chrono::milliseconds(2000));
        return error(499, "invalid_request_error", "client_closed_request", "client closed the connection",
                     rid);
    };
    const std::string cid = "chatcmpl-" + std::to_string(job.value);
    const std::int64_t created = static_cast<std::int64_t>(std::time(nullptr));
    auto model_name = [this, p]() {
        auto lm = cp_.loaded_model();
        return lm ? *lm : (p.model.empty() ? std::string("unknown") : p.model);
    };

    // Wait for the first output (or the end of the job) so failures that occur before any token
    // (model not loaded, OOM on auto-load, cancelled while queued) become proper HTTP errors.
    std::string first;
    bool got_first = false, finished = false;
    while (!got_first && !finished) {
        JobEvent ev;
        switch (channel->pop(ev, std::min(deadline, clock::now() + kPoll))) {
            case JobChannel::Pop::Event:
                if (ev.kind == JobEventKind::Output) {
                    first = std::move(ev.text);
                    got_first = true;
                }
                break;
            case JobChannel::Pop::Done:
                finished = true;
                break;
            case JobChannel::Pop::Timeout:
                if (clock::now() >= deadline) {
                    (void)cp_.cancel_job(job);
                    (void)cp_.wait_job(job, std::chrono::milliseconds(2000));
                    return error(504, "server_error", "timeout", "request timed out", rid);
                }
                if (peer_gone())
                    return abandoned();
                break;
        }
    }
    if (!finished && peer_gone())
        return abandoned();
    if (finished && !got_first) {
        auto st = cp_.job_status(job);
        if (st && (st.value().state == JobState::Failed || st.value().state == JobState::Cancelled)) {
            if (st.value().error)
                return from_error(*st.value().error, rid);
            return error(409, "invalid_request_error", "cancelled", "job was cancelled", rid);
        }
    }

    auto usage_json = [this, job]() -> json {
        auto r = cp_.chat_result(job);
        if (!r)
            return nullptr;
        return {{"prompt_tokens", r.value().prompt_tokens},
                {"completion_tokens", r.value().completion_tokens},
                {"total_tokens", r.value().prompt_tokens + r.value().completion_tokens}};
    };
    auto finish_reason = [this, job]() -> std::string {
        auto r = cp_.chat_result(job);
        return r ? r.value().finish_reason : std::string("stop");
    };

    if (!stream) {
        std::string text = std::move(first);
        while (!finished) {
            JobEvent ev;
            switch (channel->pop(ev, std::min(deadline, clock::now() + kPoll))) {
                case JobChannel::Pop::Event:
                    if (ev.kind == JobEventKind::Output)
                        text += ev.text;
                    break;
                case JobChannel::Pop::Done:
                    finished = true;
                    break;
                case JobChannel::Pop::Timeout:
                    if (clock::now() >= deadline) {
                        (void)cp_.cancel_job(job);
                        (void)cp_.wait_job(job, std::chrono::milliseconds(2000));
                        return error(504, "server_error", "timeout", "request timed out", rid);
                    }
                    break;
            }
            // Events keep flowing while a generation runs, so poll the peer on every iteration, not only on
            // idle.
            if (!finished && peer_gone())
                return abandoned();
        }
        auto st = cp_.job_status(job);
        if (st && st.value().state != JobState::Succeeded) {
            if (st.value().error)
                return from_error(*st.value().error, rid);
            return error(409, "invalid_request_error", "cancelled", "job was cancelled", rid);
        }
        json message = {{"role", "assistant"}, {"content", text}};
        std::string fr = finish_reason();
        if (tools_on) {
            auto po = chat_tools::parse_output(text, tools.tools, job.value);
            if (!po.calls.empty()) {
                message["content"] = po.content.empty() ? json(nullptr) : json(po.content);
                message["tool_calls"] = tool_calls_json(po.calls, false);
                fr = "tool_calls";
            }
        }
        json j = {{"id", cid},
                  {"object", "chat.completion"},
                  {"created", created},
                  {"model", model_name()},
                  {"choices", json::array({json{{"index", 0}, {"message", message}, {"finish_reason", fr}}})},
                  {"usage", usage_json()},
                  {"hcs", {{"job_id", to_string(job)}, {"request_id", rid}}}};
        return json_ok(j);
    }

    HttpResponse r;
    r.content_type = "text/event-stream";
    r.headers.emplace_back("X-Accel-Buffering", "no");
    r.stream = [this, guard, channel, job, cid, created, model_name, first = std::move(first), got_first,
                finished, deadline, usage_json, finish_reason, rid, peer_closed, tools_on,
                tdefs = tools.tools](const StreamWriter& write) {
        (void)guard;  // keeps the in-flight slot until streaming ends
        try {
            chat_tools::StreamSplitter splitter(tdefs, job.value);
            auto chunk = [&](const json& delta, const json& fr, const json* usage) {
                json j = {
                    {"id", cid},
                    {"object", "chat.completion.chunk"},
                    {"created", created},
                    {"model", model_name()},
                    {"choices", json::array({json{{"index", 0}, {"delta", delta}, {"finish_reason", fr}}})}};
                if (usage)
                    j["usage"] = *usage;
                return "data: " + dump_safe(j) + "\n\n";
            };
            // With tools on, text that may start a <tool_call> block is held back until it is decided.
            auto emit = [&](const std::string& t) {
                if (!tools_on)
                    return write(chunk({{"content", t}}, nullptr, nullptr));
                const std::string out = splitter.feed(t);
                return out.empty() || write(chunk({{"content", out}}, nullptr, nullptr));
            };
            if (!write(chunk({{"role", "assistant"}, {"content", ""}}, nullptr, nullptr))) {
                (void)cp_.cancel_job(job);
                return;
            }
            if (got_first && !emit(first)) {
                (void)cp_.cancel_job(job);
                return;
            }
            bool done = finished;
            bool failed = false;
            std::string fail_msg;
            while (!done) {
                JobEvent ev;
                switch (channel->pop(ev, std::min(deadline, std::chrono::steady_clock::now() +
                                                                std::chrono::milliseconds(150)))) {
                    case JobChannel::Pop::Event:
                        if (ev.kind == JobEventKind::Output && !emit(ev.text)) {
                            (void)cp_.cancel_job(job);  // client disconnected: stop the real work
                            return;
                        }
                        break;
                    case JobChannel::Pop::Done:
                        done = true;
                        break;
                    case JobChannel::Pop::Timeout:
                        if (std::chrono::steady_clock::now() >= deadline) {
                            (void)cp_.cancel_job(job);
                            failed = true;
                            fail_msg = "request timed out";
                            done = true;
                        } else if (peer_closed &&
                                   peer_closed()) {  // no token to write yet, but the client is gone
                            (void)cp_.cancel_job(job);
                            return;
                        }
                        break;
                }
            }
            if (!failed) {
                (void)cp_.wait_job(job, std::chrono::milliseconds(2000));
                if (auto st = cp_.job_status(job); st && st.value().state != JobState::Succeeded) {
                    failed = true;
                    fail_msg = st.value().error ? st.value().error->message : "job was cancelled";
                }
            }
            if (failed) {
                json e = {{"error",
                           {{"message", redact_paths(fail_msg)},
                            {"type", "server_error"},
                            {"code", "stream_aborted"},
                            {"request_id", rid}}}};
                (void)write("data: " + dump_safe(e) + "\n\n");
            } else {
                const json u = usage_json();
                std::string fr = finish_reason();
                if (tools_on) {
                    auto po = splitter.finish();
                    if (!po.content.empty())
                        (void)write(chunk({{"content", po.content}}, nullptr, nullptr));
                    if (!po.calls.empty()) {
                        (void)write(
                            chunk({{"tool_calls", tool_calls_json(po.calls, true)}}, nullptr, nullptr));
                        fr = "tool_calls";
                    }
                }
                (void)write(chunk(json::object(), fr, u.is_null() ? nullptr : &u));
            }
            (void)write("data: [DONE]\n\n");
        } catch (const std::exception& e) {
            // Nothing may escape into the transport thread: stop the real work and end the stream.
            log_.error("api", "stream threw", {{"what", e.what()}});
            (void)cp_.cancel_job(job);
        }
    };
    return r;
}

namespace {

json image_request_schema(bool edit, std::size_t max_prompt, std::uint32_t max_steps) {
    json s = {{"type", "object"},
              {"required", edit ? json::array({"prompt", "image"}) : json::array({"prompt"})},
              {"properties",
               {{"model", {{"type", "string"}, {"maxLength", 64}}},
                {"prompt", {{"type", "string"}, {"minLength", 1}, {"maxLength", max_prompt}}},
                {"negative_prompt", {{"type", "string"}, {"maxLength", max_prompt}}},
                {"n", {{"type", "integer"}, {"enum", json::array({1})}}},
                {"size", {{"type", "string"}, {"maxLength", 16}}},
                {"response_format", {{"type", "string"}, {"enum", json::array({"b64_json", "url"})}}},
                {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", max_steps}}},
                {"seed", {{"type", "integer"}, {"minimum", 0}}},
                {"cfg_scale", {{"type", "number"}, {"minimum", 0}, {"maximum", 30}}},
                {"preset", {{"type", "string"}, {"enum", json::array({"fast", "quality"})}}},
                {"strength", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                {"image", {{"type", "string"}}}}}};
    return s;
}

// "WIDTHxHEIGHT" (digits only, at most 5 each).
bool parse_size(const std::string& t, std::uint32_t& w, std::uint32_t& h) {
    const auto x = t.find('x');
    if (x == std::string::npos || x == 0 || x + 1 >= t.size())
        return false;
    auto num = [](const std::string& d, std::uint32_t& out) {
        if (d.empty() || d.size() > 5)
            return false;
        std::uint32_t v = 0;
        for (char c : d) {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + static_cast<std::uint32_t>(c - '0');
        }
        out = v;
        return true;
    };
    return num(t.substr(0, x), w) && num(t.substr(x + 1), h);
}

}  // namespace

HttpResponse ApiHandler::images(const HttpRequest& req, const std::string& rid,
                                const std::shared_ptr<Inflight>& guard, bool edit) {
    using clock = std::chrono::steady_clock;
    (void)guard;  // holds the in-flight slot for the whole (blocking) request
    if (!cp_.has_image_stream_engine())
        return error(501, "server_error", "unsupported",
                     "image generation/editing is not available: no image engine (stable-diffusion.cpp) is "
                     "linked into "
                     "this build",
                     rid);
    auto ct = req.headers.find("content-type");
    if (ct != req.headers.end() && lower(ct->second).rfind("multipart/", 0) == 0)
        return error(
            415, "invalid_request_error", "unsupported_media_type",
            "multipart/form-data is not supported: send application/json (the input image as a data URI in "
            "'image')",
            rid);
    if (ct == req.headers.end() || lower(ct->second).rfind("application/json", 0) != 0)
        return error(415, "invalid_request_error", "unsupported_media_type",
                     "Content-Type must be application/json", rid);
    if (req.body.size() > opt_.max_body_bytes)
        return error(413, "invalid_request_error", "payload_too_large",
                     "request body exceeds the configured limit", rid);
    if (!json_depth_ok(req.body, kMaxJsonDepth))
        return error(400, "invalid_request_error", "invalid_json", "JSON nesting too deep", rid);
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded())
        return error(400, "invalid_request_error", "invalid_json", "malformed JSON body", rid);
    if (auto v = validate_json(
            image_request_schema(edit, cp_.options().max_prompt_bytes, cp_.options().max_image_steps), body);
        !v)
        return error(400, "invalid_request_error", "invalid_request", v.error().message, rid);
    if (body.contains("mask"))
        return error(501, "server_error", "unsupported", "'mask' (inpainting) is not supported", rid);
    if (body.value("response_format", std::string("b64_json")) != "b64_json")
        return error(501, "server_error", "unsupported",
                     "response_format 'url' is not supported: images are returned inline as b64_json", rid);

    ImageParams p;
    p.model = body.value("model", "");
    p.prompt = body["prompt"].get<std::string>();
    p.negative_prompt = body.value("negative_prompt", "");
    if (body.contains("steps"))
        p.steps = body["steps"].get<std::uint32_t>();
    if (body.contains("seed"))
        p.seed = body["seed"].get<std::uint64_t>();
    if (body.contains("cfg_scale"))
        p.cfg_scale = body["cfg_scale"].get<float>();
    if (body.contains("preset"))
        p.preset = body["preset"].get<std::string>();
    if (body.contains("strength"))
        p.strength = body["strength"].get<float>();
    bool size_given = false;
    if (body.contains("size")) {
        if (!parse_size(body["size"].get<std::string>(), p.width, p.height))
            return error(400, "invalid_request_error", "invalid_request", "size must look like \"256x256\"",
                         rid);
        size_given = true;
    }
    if (edit) {
        std::string uri = body["image"].get<std::string>();
        if (uri.rfind("data:", 0) != 0)
            uri = "data:image/png;base64," + uri;  // bare base64 is accepted too
        auto img = decode_image_data_uri(uri, cp_.options().max_image_bytes);
        if (!img)
            return error(400, "invalid_request_error", "invalid_image", img.error().message, rid);
        if (!size_given) {  // default: keep the input image's size
            if (auto pr = probe_image(img.value().bytes.data(), img.value().bytes.size())) {
                p.width = pr->width;
                p.height = pr->height;
            }
        }
        p.init_image = std::move(img.value());
    }

    auto sub = cp_.submit_image(p, edit);
    if (!sub)
        return from_error(sub.error(), rid);
    const JobId job = sub.value().id;
    auto channel = sub.value().channel;
    const auto deadline = clock::now() + opt_.request_timeout;
    const auto peer_closed = req.peer_closed;
    constexpr auto kPoll = std::chrono::milliseconds(150);
    bool finished = false;
    while (!finished) {
        JobEvent ev;
        switch (channel->pop(ev, std::min(deadline, clock::now() + kPoll))) {
            case JobChannel::Pop::Event:
                break;
            case JobChannel::Pop::Done:
                finished = true;
                break;
            case JobChannel::Pop::Timeout:
                if (clock::now() >= deadline) {
                    (void)cp_.cancel_job(job);
                    (void)cp_.wait_job(job, std::chrono::milliseconds(5000));
                    return error(504, "server_error", "timeout", "request timed out", rid);
                }
                // A request whose client is gone must not keep the single engine worker rendering (audit D3).
                if (peer_closed && peer_closed()) {
                    (void)cp_.cancel_job(job);
                    (void)cp_.wait_job(job, std::chrono::milliseconds(5000));
                    return error(499, "invalid_request_error", "client_closed_request",
                                 "client closed the connection", rid);
                }
                break;
        }
    }
    auto st = cp_.job_status(job);
    if (!st)
        return from_error(st.error(), rid);
    if (st.value().state != JobState::Succeeded) {
        if (st.value().error)
            return from_error(*st.value().error, rid);
        return error(409, "invalid_request_error", "cancelled", "job was cancelled", rid);
    }
    auto img = cp_.image_result(job);
    if (!img)
        return from_error(img.error(), rid);
    const ImageResult& r = img.value();
    json hcs = {{"job_id", to_string(job)},
                {"request_id", rid},
                {"format", r.format},
                {"width", r.width},
                {"height", r.height},
                {"steps", r.steps},
                {"generation_ms", r.generation_ms}};
    if (auto lm = cp_.loaded_model())
        hcs["model"] = *lm;
    if (p.seed)
        hcs["seed"] = *p.seed;
    if (!r.preset.empty())
        hcs["preset"] = r.preset;
    json j = {{"created", static_cast<std::int64_t>(std::time(nullptr))},
              {"data", json::array({json{{"b64_json", base64_encode(r.bytes.data(), r.bytes.size())}}})},
              {"hcs", std::move(hcs)}};
    return json_ok(j);
}

}  // namespace hcs::api
