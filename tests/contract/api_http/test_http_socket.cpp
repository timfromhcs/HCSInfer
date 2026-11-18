// Real sockets: HttpServer bound to 127.0.0.1 on an ephemeral port, driven by a real HTTP client.
// The engine is FakeTextEngine (plumbing only, not inference evidence).
#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <future>
#include <thread>

#include "api_fixture.hpp"
#include "hcs/api/http_api.hpp"
#include "hcs/api/runtime.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;

namespace {

const char* kToken = "socket-token-0123456789abcdef";

struct Server {
    test::ApiFixture fx;
    ApiHandler handler;
    HttpServer server;
    int port = 0;

    static ApiOptions opts() {
        ApiOptions o;
        o.token = kToken;
        return o;
    }
    static TransportOptions topts(std::size_t max_body) {
        TransportOptions t;
        t.host = "127.0.0.1";
        t.port = 0;
        t.max_body_bytes = max_body;
        t.threads = 4;
        return t;
    }
    explicit Server(std::size_t max_body = 4096)
        : handler(*fx.cp, opts(), fx.logger), server(handler, topts(max_body), fx.logger) {
        auto p = server.start();
        EXPECT_TRUE(p.ok());
        if (p)
            port = p.value();
    }
    ~Server() { server.stop(); }
    httplib::Headers auth() const { return {{"Authorization", std::string("Bearer ") + kToken}}; }
};

json chat_body(bool stream) {
    return {{"model", "tiny"},
            {"stream", stream},
            {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}};
}

}  // namespace

TEST(ApiSocket, HealthIsReachableWithoutToken) {
    Server s;
    httplib::Client c("127.0.0.1", s.port);
    auto r = c.Get("/hcs/v1/health");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(json::parse(r->body)["status"], "ok");
    EXPECT_FALSE(r->get_header_value("X-Request-Id").empty());
}

TEST(ApiSocket, AuthIsEnforcedOverTheWire) {
    Server s;
    httplib::Client c("127.0.0.1", s.port);
    auto none = c.Get("/v1/models");
    ASSERT_TRUE(none);
    EXPECT_EQ(none->status, 401);
    auto wrong = c.Get("/v1/models", httplib::Headers{{"Authorization", "Bearer nope-nope-nope-nope"}});
    ASSERT_TRUE(wrong);
    EXPECT_EQ(wrong->status, 401);
    auto ok = c.Get("/v1/models", s.auth());
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
    EXPECT_EQ(json::parse(ok->body)["data"].size(), 2u);
}

TEST(ApiSocket, ForeignOriginRejectedOverTheWire) {
    Server s;
    httplib::Client c("127.0.0.1", s.port);
    auto h = s.auth();
    h.emplace("Origin", "http://evil.example");
    auto r = c.Get("/v1/models", h);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 403);
    auto h2 = s.auth();
    h2.emplace("Origin", "http://127.0.0.1:" + std::to_string(s.port));
    auto ok = c.Get("/v1/models", h2);
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
}

TEST(ApiSocket, OversizeBodyRejectedByTransportBeforeReadingIt) {
    Server s(1024);
    httplib::Client c("127.0.0.1", s.port);
    const std::string big(64 * 1024, 'x');
    auto r = c.Post("/v1/chat/completions", s.auth(), big, "application/json");
    // Either a clean 413 or the server closing the connection mid-upload is acceptable;
    // what matters is that the body was refused and the server survives.
    if (r) {
        EXPECT_EQ(r->status, 413);
        auto j = json::parse(r->body, nullptr, false);
        ASSERT_FALSE(j.is_discarded());
        EXPECT_EQ(j["error"]["code"], "payload_too_large");
    }
    httplib::Client c2("127.0.0.1", s.port);
    auto ok = c2.Get("/hcs/v1/health");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok->status, 200);
}

TEST(ApiSocket, ChatCompletionOverTheWire) {
    Server s;
    httplib::Client c("127.0.0.1", s.port);
    auto r = c.Post("/v1/chat/completions", s.auth(), chat_body(false).dump(), "application/json");
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    EXPECT_EQ(json::parse(r->body)["choices"][0]["message"]["content"], "Hello from the fake engine");
    auto bad = c.Post("/v1/chat/completions", s.auth(), "{oops", "application/json");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
}

TEST(ApiSocket, SseStreamsIncrementallyOverTheWire) {
    Server s;
    s.fx.engine.delay_ms = 40;  // spread chunks in time so incremental delivery is observable
    httplib::Client c("127.0.0.1", s.port);
    std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> seen;
    std::string content_type;
    httplib::Request req;
    req.method = "POST";
    req.path = "/v1/chat/completions";
    req.headers = s.auth();
    req.set_header("Content-Type", "application/json");
    req.body = chat_body(true).dump();
    req.content_receiver = [&](const char* d, size_t n, size_t, size_t) {
        seen.emplace_back(std::chrono::steady_clock::now(), std::string(d, n));
        return true;
    };
    req.response_handler = [&](const httplib::Response& res) {
        content_type = res.get_header_value("Content-Type");
        return res.status == 200;
    };
    auto r = c.send(req);
    ASSERT_TRUE(r);
    EXPECT_EQ(content_type.rfind("text/event-stream", 0), 0u);
    ASSERT_GE(seen.size(), 4u) << "stream arrived as a single blob";
    EXPECT_GE(
        std::chrono::duration_cast<std::chrono::milliseconds>(seen.back().first - seen.front().first).count(),
        100);
    std::string all;
    for (auto& entry : seen) all += entry.second;
    EXPECT_NE(all.find("data: [DONE]"), std::string::npos);
    std::string text;
    std::size_t pos = 0;
    while ((pos = all.find("data: ", pos)) != std::string::npos) {
        const auto e = all.find("\n\n", pos);
        const std::string payload = all.substr(pos + 6, e - pos - 6);
        pos = e;
        if (payload == "[DONE]")
            break;
        auto j = json::parse(payload);
        if (j["choices"][0]["delta"].contains("content"))
            text += j["choices"][0]["delta"]["content"].get<std::string>();
    }
    EXPECT_EQ(text, "Hello from the fake engine");
}

TEST(ApiSocket, ClientAbortMidStreamCancelsGeneration) {
    Server s;
    s.fx.engine.run_until_cancel = true;
    s.fx.engine.delay_ms = 5;
    {
        httplib::Client c("127.0.0.1", s.port);
        int n = 0;
        httplib::Request req;
        req.method = "POST";
        req.path = "/v1/chat/completions";
        req.headers = s.auth();
        req.set_header("Content-Type", "application/json");
        req.body = chat_body(true).dump();
        req.content_receiver = [&](const char*, size_t, size_t, size_t) {
            return ++n < 5;
        };  // abort the connection
        (void)c.send(req);
    }
    bool stopped = false;
    for (int i = 0; i < 600 && !stopped; ++i) {
        stopped = !s.fx.engine.generating.load();
        if (!stopped)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(stopped) << "engine kept running after the client disconnected";
}

TEST(ApiSocket, BindsLoopbackOnlyByDefault) {
    Server s;
    httplib::Client c("127.0.0.1", s.port);
    EXPECT_TRUE(c.Get("/hcs/v1/health"));
    EXPECT_EQ(s.handler.options().bind_host, "127.0.0.1");
    EXPECT_FALSE(s.handler.options().lan);
}

TEST(ApiSocket, ServerStopsCleanly) {
    int port = 0;
    {
        Server s;
        port = s.port;
        s.server.stop();
        s.server.stop();  // idempotent
    }
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(0, 300000);
    EXPECT_FALSE(c.Get("/hcs/v1/health"));
}

// ---- M10 audit regressions (real sockets) -----------------------------------------------------------------
namespace {

#ifdef _WIN32
using RawSock = SOCKET;
const RawSock kNoSock = INVALID_SOCKET;
RawSock raw_connect(int port) {
    RawSock s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSock)
        return kNoSock;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::closesocket(s);
        return kNoSock;
    }
    return s;
}
bool raw_send(RawSock s, const std::string& d) {
    return ::send(s, d.data(), static_cast<int>(d.size()), 0) == static_cast<int>(d.size());
}
void raw_close(RawSock s) {
    ::closesocket(s);
}
// True when the server closed the connection (orderly or reset) within `ms`.
bool raw_peer_closed_within(RawSock s, int ms) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    if (::select(0, &rd, nullptr, nullptr, &tv) <= 0)
        return false;
    char b[256];
    const int n = ::recv(s, b, sizeof(b), 0);
    return n <= 0;
}
#endif

}  // namespace

// D3: a non-streaming chat whose client disconnects must cancel the job instead of generating to max_tokens.
TEST(ApiSocketAudit, NonStreamingClientDisconnectCancelsTheJob) {
#ifndef _WIN32
    GTEST_SKIP() << "raw socket helper is Windows-only";
#else
    Server s;
    s.fx.engine.run_until_cancel = true;
    s.fx.engine.delay_ms = 2;
    const std::string body = chat_body(false).dump();
    RawSock c = raw_connect(s.port);
    ASSERT_NE(c, kNoSock);
    ASSERT_TRUE(raw_send(c, "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:" +
                                std::to_string(s.port) + "\r\nAuthorization: Bearer " + kToken +
                                "\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body));
    bool started = false;
    for (int i = 0; i < 300 && !started; ++i) {
        started = s.fx.engine.generating.load();
        if (!started)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(started) << "generation never started";
    raw_close(c);  // the client goes away without reading the (non-streaming) response
    bool stopped = false;
    for (int i = 0; i < 300 && !stopped; ++i) {
        stopped = !s.fx.engine.generating.load();
        if (!stopped)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(stopped) << "engine kept generating for a client that disconnected";
    EXPECT_TRUE(s.fx.cp->wait_job(JobId{1}, std::chrono::milliseconds(3000)));
    EXPECT_EQ(s.fx.cp->job_status(JobId{1}).value().state, JobState::Cancelled);
#endif
}

// D2: idle sockets must not starve the server. Excess connections are refused immediately (bounded pool +
// bounded queue) and sockets that never send a request are dropped after the read timeout.
namespace {
struct TinyServer {
    test::ApiFixture fx;
    ApiHandler handler;
    HttpServer server;
    int port = 0;
    static TransportOptions topts() {
        TransportOptions t;
        t.host = "127.0.0.1";
        t.port = 0;
        t.threads = 2;
        t.max_queued = 2;
        t.idle_timeout_s = 1;
        t.read_timeout_s = 1;
        return t;
    }
    TinyServer() : handler(*fx.cp, Server::opts(), fx.logger), server(handler, topts(), fx.logger) {
        auto p = server.start();
        EXPECT_TRUE(p.ok());
        if (p)
            port = p.value();
    }
    ~TinyServer() { server.stop(); }
};
}  // namespace

TEST(ApiSocketAudit, IdleConnectionFloodDoesNotStarveHealthAndIsBounded) {
#ifndef _WIN32
    GTEST_SKIP() << "raw socket helper is Windows-only";
#else
    TinyServer s;
    std::vector<RawSock> idle;
    for (int i = 0; i < 30; ++i) {
        RawSock c = raw_connect(s.port);
        if (c != kNoSock)
            idle.push_back(c);
    }
    ASSERT_GE(idle.size(), 20u);
    // At most threads + queue (= 4) idle connections are kept; the rest must be refused right away.
    int refused_fast = 0;
    for (auto c : idle)
        if (raw_peer_closed_within(c, 100))
            ++refused_fast;
    EXPECT_GE(refused_fast, static_cast<int>(idle.size()) - 6) << "the connection queue is unbounded";
    // The ones that were kept are dropped by the 1 s read timeout; then a normal request is served again.
    const auto t0 = std::chrono::steady_clock::now();
    httplib::Client c("127.0.0.1", s.port);
    c.set_read_timeout(8, 0);
    c.set_connection_timeout(8, 0);
    httplib::Result r;
    for (int i = 0; i < 40 && !(r && r->status == 200); ++i) {
        r = c.Get("/hcs/v1/health");
        if (!(r && r->status == 200))
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r && r->status == 200) << "health never answered";
    EXPECT_LT(ms, 4000) << "idle sockets starved the server for " << ms << " ms";
    for (auto c2 : idle) raw_close(c2);
#endif
}

TEST(ApiSocketAudit, SocketThatNeverSendsAnythingIsDroppedByTheReadTimeout) {
#ifndef _WIN32
    GTEST_SKIP() << "raw socket helper is Windows-only";
#else
    TinyServer s;
    RawSock c = raw_connect(s.port);
    ASSERT_NE(c, kNoSock);
    EXPECT_TRUE(raw_peer_closed_within(c, 3500))
        << "an idle socket kept a worker for more than 3.5 s (read timeout is 1 s)";
    raw_close(c);
#endif
}

// D-S2 (soak): a modest number of idle and half-written connections (below the connection budget) must not
// make the unauthenticated health route fail: it is accepted (not refused) and answered as soon as the
// first-byte timeout frees a worker, on the FIRST attempt (no client retry).
TEST(ApiSocketAudit, HealthAnswersFirstTryWhileIdleAndHalfOpenConnectionsAreHeld) {
#ifndef _WIN32
    GTEST_SKIP() << "raw socket helper is Windows-only";
#else
    test::ApiFixture fx;
    ApiHandler handler(*fx.cp, Server::opts(), fx.logger);
    TransportOptions t;
    t.host = "127.0.0.1";
    t.port = 0;
    t.threads = 8;
    t.max_queued = 8;
    t.idle_timeout_s = 1;
    t.read_timeout_s = 2;
    HttpServer server(handler, t, fx.logger);
    auto bound = server.start();
    ASSERT_TRUE(bound.ok());
    std::vector<RawSock> held;
    for (int i = 0; i < 12; ++i) {  // 12 <= threads + max_queued - 1 (one slot left for health)
        RawSock c = raw_connect(bound.value());
        ASSERT_NE(c, kNoSock);
        if (i % 3 == 0)
            raw_send(
                c,
                "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100000\r\n\r\n{");
        held.push_back(c);
    }
    httplib::Client c("127.0.0.1", bound.value());
    c.set_connection_timeout(5, 0);
    c.set_read_timeout(8, 0);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = c.Get("/hcs/v1/health");  // exactly one attempt
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(r) << "health connection was refused or reset while idle sockets were held";
    EXPECT_EQ(r->status, 200);
    EXPECT_LT(ms, 6000);
    for (auto s2 : held) raw_close(s2);
    server.stop();
#endif
}

// D-S2 with the REAL `serve` transport budget: 64 idle / half-written local sockets (what the soak harness
// holds) must not stop GET /hcs/v1/health on its first attempt.
TEST(ApiSocketAudit, ServeBudgetKeepsHealthAnsweringUnder64HeldConnections) {
#ifndef _WIN32
    GTEST_SKIP() << "raw socket helper is Windows-only";
#else
    test::ApiFixture fx;
    ApiHandler handler(*fx.cp, Server::opts(), fx.logger);
    const TransportOptions t = serve_transport_options(Server::opts(), "127.0.0.1", 0);
    EXPECT_GE(t.threads + t.max_queued, 80u);
    HttpServer server(handler, t, fx.logger);
    auto bound = server.start();
    ASSERT_TRUE(bound.ok());
    std::vector<RawSock> held;
    for (int i = 0; i < 64; ++i) {
        RawSock c = raw_connect(bound.value());
        ASSERT_NE(c, kNoSock);
        if (i % 2)
            raw_send(
                c,
                "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100000\r\n\r\n{");
        held.push_back(c);
    }
    httplib::Client c("127.0.0.1", bound.value());
    c.set_connection_timeout(5, 0);
    c.set_read_timeout(15, 0);
    auto r = c.Get("/hcs/v1/health");  // exactly one attempt
    ASSERT_TRUE(r) << "health connection was refused or reset while 64 sockets were held";
    EXPECT_EQ(r->status, 200);
    for (auto s2 : held) raw_close(s2);
    server.stop();
#endif
}

// D-S3 (soak): graceful shutdown during a running generation must cancel it and return promptly, not wait for
// the generation (here: endless until cancelled) or the per-request timeout.
TEST(ApiSocketAudit, GracefulShutdownCancelsRunningGenerationPromptly) {
    test::TempDir dir;
    const auto root = dir.path() / "models";
    test::write_file(root / "tiny.gguf", test::fake_gguf());
    std::ostringstream sink;
    Logger logger(&sink, LogLevel::Debug);
    RuntimeOptions ro;
    ro.state_dir = dir.path() / "state";
    std::filesystem::create_directories(ro.state_dir);
    ro.model_roots = {root.string()};
    auto eng = std::make_unique<test::FakeTextEngine>();
    eng->run_until_cancel = true;
    eng->delay_ms = 2;
    test::FakeTextEngine* fake = eng.get();
    ro.engine = std::move(eng);
    auto created = Runtime::create(std::move(ro), logger);
    ASSERT_TRUE(created.ok());
    auto rt = std::move(created.value());
    ASSERT_TRUE(rt->registry.scan(root, rt->guard).ok());
    const auto models = rt->registry.list();
    ASSERT_FALSE(models.empty());
    ApiOptions ao;
    ao.token = kToken;
    ao.request_timeout = std::chrono::seconds(300);  // the shutdown must not depend on this
    ApiHandler handler(*rt->control, ao, logger);
    HttpServer server(handler, serve_transport_options(ao, "127.0.0.1", 0), logger);
    auto bound = server.start();
    ASSERT_TRUE(bound.ok());

    std::atomic<bool> client_done{false};
    std::thread client([&] {
        httplib::Client c("127.0.0.1", bound.value());
        c.set_read_timeout(120, 0);
        const json body = {{"model", models.front().id},
                           {"stream", false},
                           {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}};
        (void)c.Post("/v1/chat/completions",
                     httplib::Headers{{"Authorization", std::string("Bearer ") + kToken}}, body.dump(),
                     "application/json");
        client_done = true;
    });
    for (int i = 0; i < 500 && !fake->generating; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(fake->generating) << "generation never started";

    const auto t0 = std::chrono::steady_clock::now();
    auto fut = std::async(std::launch::async, [&] { graceful_shutdown(server, *rt); });
    const bool finished = fut.wait_for(std::chrono::seconds(15)) == std::future_status::ready;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (!finished) {
        // Unblock so the test process can exit; the failure below is what matters.
        rt->queue->shutdown();
        fut.wait();
    }
    EXPECT_TRUE(finished) << "graceful shutdown still blocked after 15 s (waits for the running generation)";
    EXPECT_LT(ms, 15000);
    client.join();
    EXPECT_TRUE(client_done.load());
}
