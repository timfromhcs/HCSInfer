// Negative tests of the socket transport (HttpServer over cpp-httplib) with a raw TCP client (Winsock on
// Windows, BSD sockets elsewhere): oversized headers, malformed request lines, bad Content-Length, pipelining
// and slowloris. FakeTextEngine = plumbing only.
#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "api_fixture.hpp"
#include "hcs/api/http_api.hpp"

using namespace hcs;
using namespace hcs::api;
using namespace std::chrono_literals;

namespace {

#ifdef _WIN32
// Winsock comes in through <httplib.h>.
inline int nfds_for(SOCKET) {
    return 0;  // ignored by Winsock's select()
}
constexpr int kSendFlags = 0;
#else
using SOCKET = int;  // INVALID_SOCKET (-1) is defined by <httplib.h> on POSIX
inline int closesocket(SOCKET s) {
    return ::close(s);
}
inline int nfds_for(SOCKET s) {
    return s + 1;
}
constexpr int kSendFlags = MSG_NOSIGNAL;  // a peer that closed must not raise SIGPIPE in the test process
#endif

const char* kTok = "negative-token-0123456789abcdef";

struct Srv {
    test::ApiFixture fx;
    ApiHandler handler;
    HttpServer server;
    int port = 0;

    static ApiOptions opts() {
        ApiOptions o;
        o.token = kTok;
        return o;
    }
    static TransportOptions topts(int deadline_s) {
        TransportOptions t;
        t.host = "127.0.0.1";
        t.port = 0;
        t.max_body_bytes = 4096;
        t.threads = 4;
        t.max_queued = 4;
        t.idle_timeout_s = 1;
        t.read_timeout_s = 2;
        t.request_deadline_s = deadline_s;
        return t;
    }
    explicit Srv(int deadline_s = 4)
        : handler(*fx.cp, opts(), fx.logger), server(handler, topts(deadline_s), fx.logger) {
        auto p = server.start();
        EXPECT_TRUE(p.ok());
        if (p)
            port = p.value();
    }
    ~Srv() { server.stop(); }
};

SOCKET connect_raw(int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool send_all(SOCKET s, const std::string& d) {
    std::size_t off = 0;
    while (off < d.size()) {
        const int n = ::send(s, d.data() + off, static_cast<int>(d.size() - off), kSendFlags);
        if (n <= 0)
            return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// Reads until the peer closes or `total_ms` elapsed; returns everything received and whether it closed.
struct Reply {
    std::string data;
    bool closed = false;
};
Reply read_reply(SOCKET s, int total_ms) {
    Reply r;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);
    while (std::chrono::steady_clock::now() < end) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{0, 50 * 1000};
        if (::select(nfds_for(s), &rd, nullptr, nullptr, &tv) <= 0)
            continue;
        char b[4096];
        const int n = ::recv(s, b, sizeof(b), 0);
        if (n <= 0) {
            r.closed = true;
            break;
        }
        r.data.append(b, static_cast<std::size_t>(n));
    }
    return r;
}

Reply roundtrip(int port, const std::string& payload, int total_ms = 3000) {
    SOCKET s = connect_raw(port);
    Reply r;
    if (s == INVALID_SOCKET)
        return r;
    (void)send_all(s, payload);
    r = read_reply(s, total_ms);
    ::closesocket(s);
    return r;
}

int status_of(const std::string& reply) {
    if (reply.rfind("HTTP/1.", 0) != 0 || reply.size() < 12)
        return 0;
    return std::atoi(reply.c_str() + 9);
}

std::size_t count_status_lines(const std::string& reply) {
    std::size_t n = 0, pos = 0;
    while ((pos = reply.find("HTTP/1.1 ", pos)) != std::string::npos) {
        ++n;
        pos += 9;
    }
    return n;
}

void expect_alive(int port) {
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(5, 0);
    c.set_read_timeout(8, 0);
    auto r = c.Get("/hcs/v1/health");
    ASSERT_TRUE(r) << "server stopped answering after a malformed request";
    EXPECT_EQ(r->status, 200);
}

const std::string kHost = "Host: 127.0.0.1\r\n";

}  // namespace

TEST(TransportNegative, OversizedHeaderIsRejectedAndServerSurvives) {
    Srv s;
    const std::string huge(200 * 1024, 'A');
    const auto r =
        roundtrip(s.port, "GET /hcs/v1/health HTTP/1.1\r\n" + kHost + "X-Big: " + huge + "\r\n\r\n");
    const int st = status_of(r.data);
    EXPECT_TRUE(st == 0 || st == 400 || st == 413 || st == 414 || st == 431)
        << "a 200 KB header line was served with status " << st;
    // Many moderate header lines instead of one huge one.
    std::string many = "GET /hcs/v1/health HTTP/1.1\r\n" + kHost;
    for (int i = 0; i < 5000; ++i) many += "X-H" + std::to_string(i) + ": v\r\n";
    many += "\r\n";
    const auto r2 = roundtrip(s.port, many);
    const int st2 = status_of(r2.data);
    EXPECT_TRUE(st2 == 0 || st2 >= 400) << "5000 header lines were served with status " << st2;
    // Very long request target.
    const auto r3 =
        roundtrip(s.port, "GET /" + std::string(100 * 1024, 'p') + " HTTP/1.1\r\n" + kHost + "\r\n");
    const int st3 = status_of(r3.data);
    EXPECT_TRUE(st3 == 0 || st3 == 404 || st3 >= 400) << st3;
    EXPECT_NE(st3, 200);
    expect_alive(s.port);
}

TEST(TransportNegative, MalformedRequestLinesAreNeverServedAsSuccess) {
    Srv s;
    const std::string cases[] = {
        "GARBAGE\r\n\r\n",
        "GET\r\n\r\n",
        "GET /hcs/v1/health\r\n\r\n",  // HTTP/0.9 style, no version
        "GET /hcs/v1/health HTTP/9.9\r\n" + kHost + "\r\n",
        "GET  /hcs/v1/health  HTTP/1.1\r\n" + kHost + "\r\n",  // double spaces
        std::string("\x00\x01\x02\xff\xfe GET / HTTP/1.1\r\n\r\n", 24),
        "GET /hcs/v1/health HTTP/1.1\nHost: 127.0.0.1\n\n",  // bare LF framing
        "\r\n\r\nGET /hcs/v1/health HTTP/1.1\r\n" + kHost + "\r\n",
    };
    for (const auto& c : cases) {
        const auto r = roundtrip(s.port, c, 2500);
        const int st = status_of(r.data);
        // A tolerant parse of bare-LF / leading blank lines may legitimately answer 200 for the real health
        // request; what must never happen is a 5xx, a crash or a hang. Pure garbage must not be a 200.
        EXPECT_LT(st, 500) << "request: " << c.substr(0, 30);
        if (c.rfind("GARBAGE", 0) == 0 || c == "GET\r\n\r\n") {
            EXPECT_NE(st, 200);
        }
    }
    expect_alive(s.port);
}

TEST(TransportNegative, BadContentLengthIsRejectedOrIgnoredSafely) {
    Srv s;
    const std::string auth = std::string("Authorization: Bearer ") + kTok + "\r\n";
    const std::string base =
        "POST /v1/chat/completions HTTP/1.1\r\n" + kHost + auth + "Content-Type: application/json\r\n";
    const std::string bad_lengths[] = {"abc", "-1", "99999999999999999999999999", "1e3", "0x10", "5 5", ""};
    for (const auto& cl : bad_lengths) {
        const auto r =
            roundtrip(s.port, base + "Content-Length: " + cl + "\r\nConnection: close\r\n\r\n{}", 2500);
        const int st = status_of(r.data);
        EXPECT_TRUE(st == 0 || st >= 400) << "Content-Length '" << cl << "' was accepted with status " << st;
        EXPECT_NE(st, 200);
    }
    // Declared larger than the transport cap: refused before the body is read.
    const auto big = roundtrip(s.port, base + "Content-Length: 1000000\r\nConnection: close\r\n\r\n{}", 2500);
    const int bst = status_of(big.data);
    EXPECT_TRUE(bst == 0 || (bst >= 400 && bst < 500)) << bst;
    // Duplicate, conflicting Content-Length headers.
    const auto dup = roundtrip(
        s.port, base + "Content-Length: 2\r\nContent-Length: 40\r\nConnection: close\r\n\r\n{}", 2500);
    const int dst = status_of(dup.data);
    EXPECT_TRUE(dst == 0 || dst >= 400 || dst == 200) << dst;  // never a crash; handler validates the body
    // Declared longer than sent, then the client goes silent: the read timeout frees the worker.
    const auto t0 = std::chrono::steady_clock::now();
    const auto lie = roundtrip(s.port, base + "Content-Length: 100\r\n\r\n{}", 6000);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 7s);
    EXPECT_TRUE(lie.closed || status_of(lie.data) >= 400)
        << "a body that never completed kept the worker busy";
    expect_alive(s.port);
}

TEST(TransportNegative, PipelinedRequestsAreAnsweredInOrderOrDroppedNeverReordered) {
    // cpp-httplib serves the first pipelined request and does not read ahead; HTTP/1.1 clients are not
    // required to pipeline. The contract: the first request is answered, anything that IS answered afterwards
    // is in request order with the right auth outcome, and the server keeps serving.
    Srv s;
    const std::string two = "GET /hcs/v1/health HTTP/1.1\r\n" + kHost + "\r\n" +
                            "GET /v1/models HTTP/1.1\r\n" + kHost + "\r\n" +
                            "GET /hcs/v1/health HTTP/1.1\r\n" + kHost + "Connection: close\r\n\r\n";
    const auto r = roundtrip(s.port, two, 4000);
    ASSERT_GE(count_status_lines(r.data), 1u) << "the first pipelined request was not answered";
    EXPECT_EQ(status_of(r.data), 200);
    const auto second = r.data.find("HTTP/1.1 ", 9);
    if (second != std::string::npos) {
        EXPECT_EQ(std::atoi(r.data.c_str() + second + 9), 401)
            << "second answer is not the unauthenticated /v1/models";
    }
    EXPECT_LE(count_status_lines(r.data), 3u);
    expect_alive(s.port);
}

TEST(TransportNegative, RequestBodyIsNotReinterpretedAsAPipelinedRequest) {
    Srv s;
    // The POST body looks like a second request; only the real Content-Length bytes belong to the body.
    const std::string smuggled = "GET /hcs/v1/runtime HTTP/1.1\r\n" + kHost + "\r\n";
    const std::string body = "{\"model\":\"x\"}";
    const std::string auth = std::string("Authorization: Bearer ") + kTok + "\r\n";
    const std::string req =
        "POST /v1/chat/completions HTTP/1.1\r\n" + kHost + auth +
        "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body + smuggled;
    const auto r = roundtrip(s.port, req, 3000);
    EXPECT_LE(count_status_lines(r.data), 1u) << "the bytes after the body were executed as a second request";
    expect_alive(s.port);
}

TEST(TransportNegative, SlowlorisHeaderDripIsCutOffInBoundedTime) {
    Srv s;
    SOCKET c = connect_raw(s.port);
    ASSERT_NE(c, INVALID_SOCKET);
    // One header byte every 600 ms (< the 2 s per-read timeout, so a per-read timer alone never fires).
    const std::string head = "GET /hcs/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Slow: ";
    const auto t0 = std::chrono::steady_clock::now();
    bool cut = false;
    std::size_t sent = 0;
    while (std::chrono::steady_clock::now() - t0 < 16s) {
        const std::string piece = sent < head.size() ? head.substr(sent, 1) : "a";
        if (!send_all(c, piece)) {
            cut = true;
            break;
        }
        ++sent;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(c, &rd);
        timeval tv{0, 600 * 1000};
        if (::select(nfds_for(c), &rd, nullptr, nullptr, &tv) > 0) {
            char b[256];
            const int n = ::recv(c, b, sizeof(b), 0);
            if (n <= 0) {
                cut = true;
                break;
            }
        }
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ::closesocket(c);
    EXPECT_TRUE(cut) << "a client dripping header bytes kept a worker for " << ms
                     << " ms (no overall header deadline)";
    expect_alive(s.port);
}

// The request deadline covers only RECEIVING a request. A request that is complete may run (and stream) far
// longer than the deadline, and the next request on the same connection gets a fresh deadline.
TEST(TransportNegative, LongRunningHandlerAndStreamAreNotKilledByTheRequestDeadline) {
    Srv s(1);                    // 1 s deadline
    s.fx.engine.delay_ms = 400;  // 5 pieces => ~2 s per generation, twice the deadline
    httplib::Client c("127.0.0.1", s.port);
    c.set_read_timeout(30, 0);
    const httplib::Headers auth{{"Authorization", std::string("Bearer ") + kTok}};
    const std::string body_plain =
        R"({"model":"tiny","stream":false,"messages":[{"role":"user","content":"hi"}]})";
    const std::string body_stream =
        R"({"model":"tiny","stream":true,"messages":[{"role":"user","content":"hi"}]})";
    auto a = c.Post("/v1/chat/completions", auth, body_plain, "application/json");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->status, 200) << a->body;
    EXPECT_NE(a->body.find("Hello from the fake engine"), std::string::npos);
    // Same keep-alive connection, streamed this time.
    auto b = c.Post("/v1/chat/completions", auth, body_stream, "application/json");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->status, 200);
    EXPECT_NE(b->body.find("[DONE]"), std::string::npos) << "the stream was cut by the request deadline";
    expect_alive(s.port);
}

// Concurrency limit over a real socket (wave 3 test gap): with max_inflight = 1 a second inference request is
// refused with 429 + Retry-After while health and cancel stay available, and the slot is released afterwards.
TEST(TransportNegative, ConcurrencyLimitOverTheWireReturns429ButHealthAndCancelStayUp) {
    test::ApiFixture fx;
    ApiOptions ao;
    ao.token = kTok;
    ao.max_inflight = 1;
    ApiHandler handler(*fx.cp, ao, fx.logger);
    HttpServer server(handler, Srv::topts(30), fx.logger);
    auto bound = server.start();
    ASSERT_TRUE(bound.ok());
    fx.engine.run_until_cancel = true;
    fx.engine.delay_ms = 2;
    const httplib::Headers auth{{"Authorization", std::string("Bearer ") + kTok}};
    const std::string body = R"({"model":"tiny","stream":false,"messages":[{"role":"user","content":"hi"}]})";
    std::thread first([&] {
        httplib::Client c("127.0.0.1", bound.value());
        c.set_read_timeout(60, 0);
        (void)c.Post("/v1/chat/completions", auth, body, "application/json");
    });
    for (int i = 0; i < 500 && !fx.engine.generating.load(); ++i) std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(fx.engine.generating.load());

    httplib::Client c2("127.0.0.1", bound.value());
    auto second = c2.Post("/v1/chat/completions", auth, body, "application/json");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 429);
    EXPECT_FALSE(second->get_header_value("Retry-After").empty());
    auto health = c2.Get("/hcs/v1/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    auto cancel = c2.Post("/hcs/v1/jobs/job-1/cancel", auth, "", "application/json");
    ASSERT_TRUE(cancel);
    EXPECT_EQ(cancel->status, 200);
    first.join();
    auto after = c2.Get("/v1/models", auth);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->status, 200) << "the inflight slot was not released";
    server.stop();
}
