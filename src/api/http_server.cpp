// Socket transport. cpp-httplib is confined to this translation unit (compiled with relaxed
// warnings: it is third-party code); everything security-relevant lives in ApiHandler.
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <thread>

#include "hcs/api/http_api.hpp"

namespace hcs::api {

struct HttpServer::Impl {
    Impl(ApiHandler& h, TransportOptions o, Logger& l) : handler(h), opt(std::move(o)), log(l) {}
    ApiHandler& handler;
    TransportOptions opt;
    Logger& log;
    httplib::Server svr;
    std::thread thread;
    std::atomic<bool> running{false};
    int port = 0;
};

namespace {

std::string lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

HttpRequest convert(const httplib::Request& r) {
    HttpRequest out;
    out.method = r.method;
    out.path = r.path;
    for (const auto& [k, v] : r.headers) {
        const std::string key = lower_copy(k);
        auto it = out.headers.find(key);
        if (it == out.headers.end()) {
            out.headers.emplace(key, v);
        } else if (key == "host" || key == "authorization" || key == "origin" || key == "content-type") {
            it->second =
                "\x01"
                "duplicate-header";  // ambiguous security header: guaranteed to fail validation
        }
    }
    out.body = r.body;
    out.remote_addr = r.remote_addr;
    // Lets the handler notice an abandoned request while it waits for a long job (M10 audit D3). The probe is
    // only called on the connection's own worker thread while the request is being served.
    if (r.is_connection_closed)
        out.peer_closed = r.is_connection_closed;
    return out;
}

void apply(HttpResponse resp, httplib::Response& res) {
    res.status = resp.status;
    for (const auto& [k, v] : resp.headers) res.set_header(k, v);
    if (resp.stream) {
        auto fn = std::make_shared<std::function<void(const StreamWriter&)>>(std::move(resp.stream));
        res.set_chunked_content_provider(resp.content_type, [fn](std::size_t, httplib::DataSink& sink) {
            (*fn)([&sink](std::string_view chunk) {
                return sink.is_writable() && sink.write(chunk.data(), chunk.size());
            });
            sink.done();
            return true;
        });
    } else {
        res.set_content(resp.body, resp.content_type);
    }
}

}  // namespace

HttpServer::HttpServer(ApiHandler& handler, TransportOptions options, Logger& logger)
    : impl_(std::make_unique<Impl>(handler, std::move(options), logger)) {}

HttpServer::~HttpServer() {
    stop();
}

Result<int> HttpServer::start() {
    auto& s = *impl_;
    if (s.running.load())
        return make_error(ErrorCode::InvalidState, "server already running");
    const auto threads = std::max<std::size_t>(1, s.opt.threads);
    const auto queued = std::max<std::size_t>(1, s.opt.max_queued);
    // httplib::ThreadPool(n, max_n, mqr): n fixed workers, max_n == n (no dynamic threads) and a bounded
    // queue of `mqr` waiting connections. A connection that finds the queue full is closed immediately, so
    // the socket backlog can no longer grow without limit and idle sockets cannot starve the server (M10
    // audit D2: the old call ThreadPool(threads, queued) put `queued` into max_n and left the queue
    // unbounded).
    s.svr.new_task_queue = [threads, queued] { return new httplib::ThreadPool(threads, threads, queued); };
    s.svr.set_payload_max_length(s.opt.max_body_bytes);
    s.svr.set_read_timeout(s.opt.read_timeout_s, 0);
    s.svr.set_write_timeout(s.opt.write_timeout_s, 0);
    s.svr.set_keep_alive_max_count(16);
    // httplib waits for the first byte of every request (also the first one) under this timeout, not under
    // the read timeout: a socket that connects and says nothing is dropped after idle_timeout_s.
    s.svr.set_keep_alive_timeout(std::max(1, s.opt.idle_timeout_s));

    auto handler = [&s](const httplib::Request& req, httplib::Response& res) {
        apply(s.handler.handle(convert(req)), res);
    };
    s.svr.Get(".*", handler);
    s.svr.Post(".*", handler);
    s.svr.Put(".*", handler);
    s.svr.Delete(".*", handler);
    s.svr.Patch(".*", handler);
    s.svr.Options(".*", handler);
    // httplib-generated errors (413 payload too large, 400 bad request line, ...): JSON body.
    s.svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.body.empty()) {
            const std::string code = res.status == 413 ? "payload_too_large" : "http_error";
            res.set_content(
                "{\"error\":{\"message\":\"request rejected by the HTTP "
                "layer\",\"type\":\"invalid_request_error\","
                "\"code\":\"" +
                    code + "\",\"param\":null}}",
                "application/json");
        }
    });

    int port = s.opt.port;
    if (port == 0) {
        port = s.svr.bind_to_any_port(s.opt.host);
        if (port < 0)
            return make_error(ErrorCode::IoError, "cannot bind " + s.opt.host);
    } else if (!s.svr.bind_to_port(s.opt.host, port)) {
        return make_error(ErrorCode::IoError, "cannot bind " + s.opt.host + ":" + std::to_string(port) +
                                                  " (port in use or address not available)");
    }
    s.port = port;
    s.handler.set_port(port);
    s.running = true;
    s.thread = std::thread([&s] {
        s.svr.listen_after_bind();
        s.running = false;
    });
    // Wait until the accept loop is live so callers can connect immediately.
    for (int i = 0; i < 500 && !s.svr.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    s.log.info("api", "listening", {{"host", s.opt.host}, {"port", std::to_string(port)}});
    return port;
}

void HttpServer::stop() {
    if (!impl_)
        return;
    impl_->svr.stop();
    if (impl_->thread.joinable())
        impl_->thread.join();
    impl_->running = false;
}

void HttpServer::wait() {
    if (impl_ && impl_->thread.joinable())
        impl_->thread.join();
}

}  // namespace hcs::api
