#pragma once
// Framework-neutral HTTP API handler (AGENTS.md 14.3) plus the socket transport.
//   ApiHandler  : request -> response; owns auth, Host/Origin checks, validation, routing.
//   HttpServer  : binds a socket (cpp-httplib, confined to one translation unit) and feeds ApiHandler.
// Splitting them keeps every security rule unit-testable without sockets.
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hcs/api/control_plane.hpp"

namespace hcs::api {

struct HttpRequest {
    std::string method;                          // upper-case
    std::string path;                            // without query string
    std::string query;                           // raw, without '?'
    std::map<std::string, std::string> headers;  // keys lower-cased
    std::string body;
    std::string remote_addr;
    // Optional transport probe: returns true once the peer has closed the connection. The handler polls it
    // while it waits for a long job so an abandoned request cancels its job instead of generating on (M10
    // audit D3). Empty = unknown (never treated as closed).
    std::function<bool()> peer_closed;
};

using StreamWriter = std::function<bool(std::string_view chunk)>;  // false => client went away

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // When set, status/headers are sent first and the function then produces the body by
    // calling the writer; it runs on the transport thread and must return when done.
    std::function<void(const StreamWriter&)> stream;
};

struct ApiOptions {
    std::string token;  // required: every route except /hcs/v1/health needs "Authorization: Bearer <token>"
    int port = 0;       // actual listening port (set by the server after bind)
    bool lan = false;   // explicit LAN opt-in: IP-literal Host headers are accepted too
    std::string bind_host = "127.0.0.1";
    std::vector<std::string> allowed_hosts;    // extra Host names (hostname only, lower-case)
    std::vector<std::string> allowed_origins;  // extra full origins, e.g. "http://127.0.0.1:3000"
    std::size_t max_body_bytes = std::size_t{1} * 1024 * 1024;
    std::size_t max_inflight = 8;
    std::chrono::milliseconds request_timeout{300000};
};

// Schemas advertised in docs/api.md and enforced at runtime.
nlohmann::json chat_completions_schema(std::uint32_t max_tokens_cap);

class ApiHandler {
public:
    ApiHandler(ControlPlane& cp, ApiOptions options, Logger& logger);
    HttpResponse handle(const HttpRequest& req);
    void set_port(int port);
    const ApiOptions& options() const noexcept { return opt_; }

    // Exposed for tests.
    bool host_allowed(const std::string& host_header) const;
    bool origin_allowed(const std::string& origin_header) const;

private:
    struct Inflight;
    HttpResponse dispatch(const HttpRequest& req, const std::string& rid,
                          const std::shared_ptr<Inflight>& guard);
    HttpResponse chat(const HttpRequest& req, const std::string& rid, const std::shared_ptr<Inflight>& guard);
    HttpResponse benchmarks(const HttpRequest& req, const std::string& rid);
    HttpResponse images(const HttpRequest& req, const std::string& rid,
                        const std::shared_ptr<Inflight>& guard, bool edit);
    HttpResponse error(int status, const std::string& type, const std::string& code, const std::string& msg,
                       const std::string& rid) const;
    HttpResponse from_error(const Error& e, const std::string& rid) const;
    HttpResponse json_ok(const nlohmann::json& j, int status = 200) const;
    std::string new_request_id();

    ControlPlane& cp_;
    ApiOptions opt_;
    Logger& log_;
    std::atomic<int> port_;
    std::atomic<std::size_t> inflight_{0};
    std::mutex rng_mutex_;
    std::uint64_t rng_state_;
    nlohmann::json chat_schema_;
};

struct TransportOptions {
    std::string host = "127.0.0.1";
    int port = 8765;  // 0 => pick a free port
    std::size_t max_body_bytes = std::size_t{1} * 1024 * 1024;
    // Connection budget: at most `threads` connections are served and `max_queued` more wait for a worker;
    // any further connection is closed immediately (M10 audit D2). One idle socket therefore costs at most
    // `idle_timeout_s` of one worker, and a flood is bounded by (threads + max_queued) sockets, not by
    // memory.
    std::size_t threads = 8;
    std::size_t max_queued = 32;
    int idle_timeout_s = 2;  // wait for the first byte of a request (also the keep-alive wait)
    int read_timeout_s = 5;  // per read once bytes flow (headers, body)
    int write_timeout_s = 30;
};

class HttpServer {
public:
    HttpServer(ApiHandler& handler, TransportOptions options, Logger& logger);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    Result<int> start();  // binds and serves in a background thread; returns the bound port
    void stop();          // idempotent
    void wait();          // blocks until stopped

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Random 256-bit token (hex), from the OS CSPRNG-backed std::random_device.
std::string generate_token();

}  // namespace hcs::api
