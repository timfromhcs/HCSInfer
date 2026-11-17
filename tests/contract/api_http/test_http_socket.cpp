// Real sockets: HttpServer bound to 127.0.0.1 on an ephemeral port, driven by a real HTTP client.
// The engine is FakeTextEngine (plumbing only, not inference evidence).
#include <httplib.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "api_fixture.hpp"
#include "hcs/api/http_api.hpp"

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
        if (p) port = p.value();
    }
    ~Server() { server.stop(); }
    httplib::Headers auth() const { return {{"Authorization", std::string("Bearer ") + kToken}}; }
};

json chat_body(bool stream) {
    return {{"model", "tiny"}, {"stream", stream}, {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}};
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
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(seen.back().first - seen.front().first).count(), 100);
    std::string all;
    for (auto& entry : seen) all += entry.second;
    EXPECT_NE(all.find("data: [DONE]"), std::string::npos);
    std::string text;
    std::size_t pos = 0;
    while ((pos = all.find("data: ", pos)) != std::string::npos) {
        const auto e = all.find("\n\n", pos);
        const std::string payload = all.substr(pos + 6, e - pos - 6);
        pos = e;
        if (payload == "[DONE]") break;
        auto j = json::parse(payload);
        if (j["choices"][0]["delta"].contains("content")) text += j["choices"][0]["delta"]["content"].get<std::string>();
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
        req.content_receiver = [&](const char*, size_t, size_t, size_t) { return ++n < 5; };  // abort the connection
        (void)c.send(req);
    }
    bool stopped = false;
    for (int i = 0; i < 600 && !stopped; ++i) {
        stopped = !s.fx.engine.generating.load();
        if (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
