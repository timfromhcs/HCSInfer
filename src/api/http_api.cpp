#include "hcs/api/http_api.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <exception>
#include <random>
#include <sstream>

#include "hcs/api/data_uri.hpp"
#include "hcs/api/json_schema.hpp"

namespace hcs::api {
namespace {

using json = nlohmann::json;

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
        case ErrorCode::Ok: return 200;
        case ErrorCode::InvalidArgument: return 400;
        case ErrorCode::ModelError: return 422;
        case ErrorCode::BackendError: return 502;
        case ErrorCode::OutOfMemory: return 503;
        case ErrorCode::Cancelled: return 409;
        case ErrorCode::NotFound: return 404;
        case ErrorCode::IoError: return 500;
        case ErrorCode::SecurityViolation: return 403;
        case ErrorCode::Unsupported: return 501;
        case ErrorCode::InvalidState: return 409;
        case ErrorCode::AlreadyExists: return 409;
        case ErrorCode::Busy: return 429;
        case ErrorCode::Timeout: return 504;
        case ErrorCode::Internal: return 500;
    }
    return 500;
}

std::string type_for(int status) {
    if (status == 401) return "authentication_error";
    if (status == 403) return "permission_error";
    if (status == 404) return "not_found_error";
    if (status == 429) return "rate_limit_error";
    if (status >= 500) return "server_error";
    return "invalid_request_error";
}

// Maximum bracket nesting outside of strings. Cheap guard against pathological bodies.
bool json_depth_ok(const std::string& s, int max_depth) {
    int depth = 0;
    bool in_str = false, esc = false;
    for (char c : s) {
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{' || c == '[') {
            if (++depth > max_depth) return false;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return true;
}

std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < p.size()) {
        if (p[i] == '/') { ++i; continue; }
        std::size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
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
    if (h.empty()) return false;
    for (char c : h) {
        if (c == '.') ++dots;
        else if (c < '0' || c > '9') return false;
    }
    return dots == 3 && h.front() != '.' && h.back() != '.' && h.find("..") == std::string::npos;
}

bool is_ipv6_literal(const std::string& h) { return h.size() >= 3 && h.front() == '[' && h.back() == ']'; }

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
        "required": ["role", "content"],
        "properties": {
          "role": {"type": "string", "enum": ["system", "user", "assistant"]},
          "content": {"anyOf": [
            {"type": "string"},
            {"type": "array", "maxItems": 64,
             "items": {"type": "object", "required": ["type"], "properties": {"type": {"type": "string"}}}}
          ]}
        }
      }
    },
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

void ApiHandler::set_port(int port) { port_.store(port); }

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
    if (host_header.empty() || host_header.size() > 255) return false;
    const std::string h = host_part(host_header);
    if (h == "localhost" || h == "127.0.0.1" || h == "[::1]") return true;
    if (h == lower(opt_.bind_host) || h == "[" + lower(opt_.bind_host) + "]") return true;
    for (const auto& a : opt_.allowed_hosts)
        if (h == lower(a)) return true;
    // LAN opt-in: IP literals cannot be DNS-rebound, names can, so names stay allow-listed only.
    if (opt_.lan && (is_ipv4_literal(h) || is_ipv6_literal(h))) return true;
    return false;
}

bool ApiHandler::origin_allowed(const std::string& origin) const {
    if (origin.empty() || origin.size() > 255) return false;
    for (const auto& a : opt_.allowed_origins)
        if (lower(a) == lower(origin)) return true;
    const std::string o = lower(origin);
    std::string rest;
    if (o.rfind("http://", 0) == 0) rest = o.substr(7);
    else if (o.rfind("https://", 0) == 0) rest = o.substr(8);
    else return false;  // includes "null" and file:// origins
    if (rest.find('/') != std::string::npos) return false;
    if (!host_allowed(rest)) return false;
    // Same port as this server: another local web app is still a cross-origin caller.
    std::string port_text;
    if (!rest.empty() && rest[0] == '[') {
        const auto e = rest.find(']');
        if (e != std::string::npos && e + 1 < rest.size() && rest[e + 1] == ':') port_text = rest.substr(e + 2);
    } else if (const auto c = rest.find(':'); c != std::string::npos) {
        port_text = rest.substr(c + 1);
    }
    return port_text == std::to_string(port_.load());
}

HttpResponse ApiHandler::error(int status, const std::string& type, const std::string& code, const std::string& msg,
                               const std::string& rid) const {
    HttpResponse r;
    r.status = status;
    json j = {{"error", {{"message", msg}, {"type", type}, {"code", code}, {"param", nullptr}, {"request_id", rid}}}};
    r.body = j.dump();
    return r;
}

HttpResponse ApiHandler::from_error(const Error& e, const std::string& rid) const {
    const int st = http_status_for(e.code);
    auto r = error(st, type_for(st), std::string(to_string(e.code)), e.message, rid);
    if (st == 429) r.headers.emplace_back("Retry-After", "1");
    return r;
}

HttpResponse ApiHandler::json_ok(const json& j, int status) const {
    HttpResponse r;
    r.status = status;
    r.body = j.dump();
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
        } else if (!is_health && [&] {
                       const std::string a = get("authorization");
                       if (a.size() < 8 || lower(a.substr(0, 7)) != "bearer ") return true;
                       return opt_.token.empty() || !ct_equal(a.substr(7), opt_.token);
                   }()) {
            resp = error(401, "authentication_error", "unauthorized", "missing or invalid bearer token", rid);
            resp.headers.emplace_back("WWW-Authenticate", "Bearer");
        } else {
            std::shared_ptr<Inflight> guard;
            // Health and job cancellation are never throttled: cancelling is the way out of overload.
            const bool is_cancel = req.method == "POST" && req.path.rfind("/hcs/v1/jobs/", 0) == 0 &&
                                   req.path.size() > 7 && req.path.compare(req.path.size() - 7, 7, "/cancel") == 0;
            if (!is_health && !is_cancel) {
                if (inflight_.fetch_add(1) >= opt_.max_inflight) {
                    inflight_.fetch_sub(1);
                    resp = error(429, "rate_limit_error", "too_many_requests", "too many concurrent requests", rid);
                    resp.headers.emplace_back("Retry-After", "1");
                } else {
                    guard = std::make_shared<Inflight>(inflight_);
                }
            }
            if (is_health || is_cancel || guard) resp = dispatch(req, rid, guard);
        }
    } catch (const std::exception& e) {
        log_.error("api", "unhandled exception", {{"rid", rid}, {"what", e.what()}});
        resp = error(500, "server_error", "internal", "internal error", rid);
    }
    resp.headers.emplace_back("X-Request-Id", rid);
    resp.headers.emplace_back("X-Content-Type-Options", "nosniff");
    resp.headers.emplace_back("Cache-Control", "no-store");
    log_.info("api", "request",
              {{"method", req.method}, {"path", req.path}, {"status", std::to_string(resp.status)}, {"rid", rid}});
    return resp;
}

HttpResponse ApiHandler::dispatch(const HttpRequest& req, const std::string& rid, const std::shared_ptr<Inflight>& guard) {
    const auto seg = split_path(req.path);
    const bool is_get = req.method == "GET";
    const bool is_post = req.method == "POST";
    auto not_allowed = [&](const char* allow) {
        auto r = error(405, "invalid_request_error", "method_not_allowed", "method not allowed for this route", rid);
        r.headers.emplace_back("Allow", allow);
        return r;
    };
    if (req.path.empty() || req.path[0] != '/' || req.path.find("//") != std::string::npos ||
        (req.path.size() > 1 && req.path.back() == '/'))
        return error(404, "not_found_error", "not_found", "no such route", rid);

    // --- OpenAI-compatible ---
    if (req.path == "/v1/models") {
        if (!is_get) return not_allowed("GET");
        json data = json::array();
        for (const auto& v : cp_.models()) {
            if (v.manifest.kind == ModelKind::Image) continue;
            data.push_back({{"id", v.manifest.id}, {"object", "model"}, {"created", 0}, {"owned_by", "local"},
                            {"hcs", {{"state", std::string(to_string(v.state))}, {"quantization", v.manifest.quantization}}}});
        }
        return json_ok({{"object", "list"}, {"data", data}});
    }
    if (req.path == "/v1/chat/completions") {
        if (!is_post) return not_allowed("POST");
        return chat(req, rid, guard);
    }
    if (req.path == "/v1/images/generations" || req.path == "/v1/images/edits") {
        if (!is_post) return not_allowed("POST");
        return error(501, "server_error", "unsupported",
                     "image generation/editing is not available: no image engine adapter (stable-diffusion.cpp) is "
                     "implemented in this build",
                     rid);
    }

    // --- HCS native ---
    if (seg.size() >= 3 && seg[0] == "hcs" && seg[1] == "v1") {
        const std::string& a = seg[2];
        if (seg.size() == 3) {
            if (a == "health") {
                if (!is_get) return not_allowed("GET");
                return json_ok({{"status", "ok"}, {"version", cp_.options().version}});
            }
            if (a == "runtime" || a == "devices" || a == "memory" || a == "models") {
                if (!is_get) return not_allowed("GET");
                if (a == "runtime") return json_ok(cp_.runtime_json());
                if (a == "devices") return json_ok(cp_.devices_json());
                if (a == "memory") return json_ok(cp_.memory_json());
                json arr = json::array();
                for (const auto& v : cp_.models()) arr.push_back(cp_.model_json(v));
                return json_ok({{"models", arr}});
            }
            if (a == "benchmarks") {
                if (!is_post) return not_allowed("POST");
                return error(501, "server_error", "unsupported", "benchmarks are not implemented yet", rid);
            }
        }
        if (a == "models" && seg.size() == 4) {
            if (!is_get) return not_allowed("GET");
            auto v = cp_.model(seg[3]);
            if (!v) return from_error(v.error(), rid);
            return json_ok(cp_.model_json(v.value()));
        }
        if (a == "models" && seg.size() == 5 && (seg[4] == "load" || seg[4] == "unload")) {
            if (!is_post) return not_allowed("POST");
            if (seg[4] == "load") {
                auto id = cp_.submit_load(seg[3]);
                if (!id) return from_error(id.error(), rid);
                json j = {{"job_id", to_string(id.value())}, {"status_url", "/hcs/v1/jobs/" + to_string(id.value())}};
                return json_ok(j, 202);
            }
            auto v = cp_.model(seg[3]);
            if (!v) return from_error(v.error(), rid);
            const auto lm = cp_.loaded_model();
            if (!lm || *lm != seg[3])
                return error(409, "invalid_request_error", "invalid_state", "that model is not loaded", rid);
            if (auto r = cp_.unload_model(); !r) return from_error(r.error(), rid);
            return json_ok({{"id", seg[3]}, {"state", "unloaded"}});
        }
        if (a == "jobs" && (seg.size() == 4 || (seg.size() == 5 && seg[4] == "cancel"))) {
            auto id = ControlPlane::parse_job_id(seg[3]);
            if (!id) return error(400, "invalid_request_error", "invalid_argument", "malformed job id", rid);
            if (seg.size() == 4) {
                if (!is_get) return not_allowed("GET");
                auto st = cp_.job_status(*id);
                if (!st) return from_error(st.error(), rid);
                return json_ok(cp_.job_json(st.value()));
            }
            if (!is_post) return not_allowed("POST");
            if (auto r = cp_.cancel_job(*id); !r) return from_error(r.error(), rid);
            auto st = cp_.job_status(*id);
            if (!st) return from_error(st.error(), rid);
            return json_ok(cp_.job_json(st.value()));
        }
    }
    return error(404, "not_found_error", "not_found", "no such route", rid);
}

HttpResponse ApiHandler::chat(const HttpRequest& req, const std::string& rid, const std::shared_ptr<Inflight>& guard) {
    using clock = std::chrono::steady_clock;
    auto ct = req.headers.find("content-type");
    if (ct == req.headers.end() || lower(ct->second).rfind("application/json", 0) != 0)
        return error(415, "invalid_request_error", "unsupported_media_type", "Content-Type must be application/json", rid);
    if (req.body.size() > opt_.max_body_bytes)
        return error(413, "invalid_request_error", "payload_too_large", "request body exceeds the configured limit", rid);
    if (!json_depth_ok(req.body, 64))
        return error(400, "invalid_request_error", "invalid_json", "JSON nesting too deep", rid);
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) return error(400, "invalid_request_error", "invalid_json", "malformed JSON body", rid);
    if (auto v = validate_json(chat_schema_, body); !v)
        return error(400, "invalid_request_error", "invalid_request", v.error().message, rid);
    for (const char* k : {"tools", "functions", "tool_choice", "function_call"}) {
        if (body.contains(k)) return error(501, "server_error", "unsupported", std::string("'") + k + "' is not supported", rid);
    }
    if (body.contains("response_format") &&
        !(body["response_format"].is_object() && body["response_format"].contains("type") &&
          body["response_format"]["type"].is_string() && body["response_format"]["type"].get<std::string>() == "text"))
        return error(501, "server_error", "unsupported", "only response_format {\"type\":\"text\"} is supported", rid);

    ChatParams p;
    p.model = body.value("model", "");
    for (const auto& m : body["messages"]) {
        ChatMessage cm;
        cm.role = m["role"].get<std::string>();
        const auto& c = m["content"];
        if (c.is_string()) {
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
                    if (!img) return error(400, "invalid_request_error", "invalid_image", img.error().message, rid);
                    cm.images.push_back(std::move(img.value()));
                } else if (t == "input_image") {
                    return error(400, "invalid_request_error", "invalid_request",
                                 "unsupported content part type 'input_image'; use {\"type\":\"image_url\"} with a data URI", rid);
                } else {
                    return error(400, "invalid_request_error", "invalid_request", "unsupported content part type '" + t + "'", rid);
                }
            }
        }
        p.messages.push_back(std::move(cm));
    }
    if (body.contains("max_tokens")) p.max_tokens = body["max_tokens"].get<std::uint32_t>();
    else if (body.contains("max_completion_tokens")) p.max_tokens = body["max_completion_tokens"].get<std::uint32_t>();
    if (body.contains("temperature")) p.temperature = body["temperature"].get<float>();
    if (body.contains("seed")) p.seed = body["seed"].get<std::uint64_t>();
    const bool stream = body.value("stream", false);

    auto sub = cp_.submit_chat(p);
    if (!sub) return from_error(sub.error(), rid);
    const JobId job = sub.value().id;
    auto channel = sub.value().channel;
    const auto deadline = clock::now() + opt_.request_timeout;
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
        switch (channel->pop(ev, deadline)) {
            case JobChannel::Pop::Event:
                if (ev.kind == JobEventKind::Output) { first = std::move(ev.text); got_first = true; }
                break;
            case JobChannel::Pop::Done: finished = true; break;
            case JobChannel::Pop::Timeout:
                (void)cp_.cancel_job(job);
                (void)cp_.wait_job(job, std::chrono::milliseconds(2000));
                return error(504, "server_error", "timeout", "request timed out", rid);
        }
    }
    if (finished && !got_first) {
        auto st = cp_.job_status(job);
        if (st && (st.value().state == JobState::Failed || st.value().state == JobState::Cancelled)) {
            if (st.value().error) return from_error(*st.value().error, rid);
            return error(409, "invalid_request_error", "cancelled", "job was cancelled", rid);
        }
    }

    auto usage_json = [this, job]() -> json {
        auto r = cp_.chat_result(job);
        if (!r) return nullptr;
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
            switch (channel->pop(ev, deadline)) {
                case JobChannel::Pop::Event:
                    if (ev.kind == JobEventKind::Output) text += ev.text;
                    break;
                case JobChannel::Pop::Done: finished = true; break;
                case JobChannel::Pop::Timeout:
                    (void)cp_.cancel_job(job);
                    (void)cp_.wait_job(job, std::chrono::milliseconds(2000));
                    return error(504, "server_error", "timeout", "request timed out", rid);
            }
        }
        auto st = cp_.job_status(job);
        if (st && st.value().state != JobState::Succeeded) {
            if (st.value().error) return from_error(*st.value().error, rid);
            return error(409, "invalid_request_error", "cancelled", "job was cancelled", rid);
        }
        json j = {{"id", cid},
                  {"object", "chat.completion"},
                  {"created", created},
                  {"model", model_name()},
                  {"choices", json::array({json{{"index", 0},
                                                {"message", {{"role", "assistant"}, {"content", text}}},
                                                {"finish_reason", finish_reason()}}})},
                  {"usage", usage_json()},
                  {"hcs", {{"job_id", to_string(job)}, {"request_id", rid}}}};
        return json_ok(j);
    }

    HttpResponse r;
    r.content_type = "text/event-stream";
    r.headers.emplace_back("X-Accel-Buffering", "no");
    r.stream = [this, guard, channel, job, cid, created, model_name, first = std::move(first), got_first, finished,
                deadline, usage_json, finish_reason, rid](const StreamWriter& write) {
        (void)guard;  // keeps the in-flight slot until streaming ends
        auto chunk = [&](const json& delta, const json& fr, const json* usage) {
            json j = {{"id", cid},
                      {"object", "chat.completion.chunk"},
                      {"created", created},
                      {"model", model_name()},
                      {"choices", json::array({json{{"index", 0}, {"delta", delta}, {"finish_reason", fr}}})}};
            if (usage) j["usage"] = *usage;
            return "data: " + j.dump() + "\n\n";
        };
        if (!write(chunk({{"role", "assistant"}, {"content", ""}}, nullptr, nullptr))) {
            (void)cp_.cancel_job(job);
            return;
        }
        if (got_first && !write(chunk({{"content", first}}, nullptr, nullptr))) {
            (void)cp_.cancel_job(job);
            return;
        }
        bool done = finished;
        bool failed = false;
        std::string fail_msg;
        while (!done) {
            JobEvent ev;
            switch (channel->pop(ev, deadline)) {
                case JobChannel::Pop::Event:
                    if (ev.kind == JobEventKind::Output && !write(chunk({{"content", ev.text}}, nullptr, nullptr))) {
                        (void)cp_.cancel_job(job);  // client disconnected: stop the real work
                        return;
                    }
                    break;
                case JobChannel::Pop::Done: done = true; break;
                case JobChannel::Pop::Timeout:
                    (void)cp_.cancel_job(job);
                    failed = true;
                    fail_msg = "request timed out";
                    done = true;
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
            json e = {{"error", {{"message", fail_msg}, {"type", "server_error"}, {"code", "stream_aborted"}, {"request_id", rid}}}};
            (void)write("data: " + e.dump() + "\n\n");
        } else {
            const json u = usage_json();
            (void)write(chunk(json::object(), finish_reason(), u.is_null() ? nullptr : &u));
        }
        (void)write("data: [DONE]\n\n");
    };
    return r;
}

}  // namespace hcs::api
