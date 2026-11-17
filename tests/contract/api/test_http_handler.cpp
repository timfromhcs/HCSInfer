// Contract tests for the HTTP API handler (no sockets). FakeTextEngine is used strictly to exercise
// plumbing; none of these tests is evidence of inference (AGENTS.md 16.2).
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "api_fixture.hpp"
#include "hcs/api/http_api.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;

namespace {

const char* kToken = "test-token-0123456789abcdef";

struct Api {
    test::ApiFixture fx;
    ApiHandler handler;
    explicit Api(ApiOptions o = {}, bool with_engine = true, ControlPlaneOptions cpo = {})
        : fx(with_engine, cpo), handler(*fx.cp, finish(std::move(o)), fx.logger) {}

    static ApiOptions finish(ApiOptions o) {
        if (o.token.empty()) o.token = kToken;
        if (o.port == 0) o.port = 8765;
        return o;
    }

    HttpRequest req(const std::string& method, const std::string& path, const std::string& body = {}, bool auth = true) {
        HttpRequest r;
        r.method = method;
        r.path = path;
        r.body = body;
        r.headers["host"] = "127.0.0.1:8765";
        if (auth) r.headers["authorization"] = std::string("Bearer ") + kToken;
        if (!body.empty()) r.headers["content-type"] = "application/json";
        return r;
    }
    HttpResponse call(const std::string& method, const std::string& path, const std::string& body = {}, bool auth = true) {
        return handler.handle(req(method, path, body, auth));
    }
    json chat_body(const std::string& content = "hi", bool stream = false, const std::string& model = "tiny") {
        json j = {{"model", model}, {"messages", json::array({json{{"role", "user"}, {"content", content}}})}, {"stream", stream}};
        return j;
    }
};

std::string collect_stream(HttpResponse& r) {
    std::string all;
    r.stream([&](std::string_view c) {
        all.append(c);
        return true;
    });
    return all;
}

json err_of(const HttpResponse& r) { return json::parse(r.body)["error"]; }

bool wait_for(const std::function<bool()>& f, int ms = 3000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

}  // namespace

// ---- authentication -----------------------------------------------------------------------
TEST(ApiAuth, MissingTokenIs401WithChallenge) {
    Api a;
    auto r = a.call("GET", "/v1/models", {}, false);
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(err_of(r)["code"], "unauthorized");
    bool challenge = false;
    for (auto& [k, v] : r.headers) challenge = challenge || (k == "WWW-Authenticate" && v == "Bearer");
    EXPECT_TRUE(challenge);
}

TEST(ApiAuth, WrongTokenIs401) {
    Api a;
    auto q = a.req("GET", "/v1/models");
    q.headers["authorization"] = "Bearer wrong-token-0123456789abcdef";
    EXPECT_EQ(a.handler.handle(q).status, 401);
    q.headers["authorization"] = std::string("Basic ") + kToken;
    EXPECT_EQ(a.handler.handle(q).status, 401);
    q.headers["authorization"] = std::string("Bearer ");
    EXPECT_EQ(a.handler.handle(q).status, 401);
}

TEST(ApiAuth, EveryRouteExceptHealthNeedsAuth) {
    Api a;
    for (auto [m, p] : std::vector<std::pair<std::string, std::string>>{
             {"GET", "/v1/models"}, {"POST", "/v1/chat/completions"}, {"POST", "/v1/images/generations"},
             {"POST", "/v1/images/edits"}, {"GET", "/hcs/v1/runtime"}, {"GET", "/hcs/v1/devices"},
             {"GET", "/hcs/v1/memory"}, {"GET", "/hcs/v1/models"}, {"POST", "/hcs/v1/models/tiny/load"},
             {"POST", "/hcs/v1/models/tiny/unload"}, {"GET", "/hcs/v1/jobs/job-1"}, {"POST", "/hcs/v1/jobs/job-1/cancel"},
             {"GET", "/does/not/exist"}}) {
        EXPECT_EQ(a.call(m, p, {}, false).status, 401) << m << " " << p;
    }
    EXPECT_EQ(a.call("GET", "/hcs/v1/health", {}, false).status, 200);
}

TEST(ApiAuth, EmptyConfiguredTokenNeverAuthenticates) {
    test::ApiFixture fx;
    ApiOptions bare;
    bare.port = 8765;
    ApiHandler h(*fx.cp, bare, fx.logger);
    HttpRequest r;
    r.method = "GET";
    r.path = "/v1/models";
    r.headers["host"] = "127.0.0.1:8765";
    r.headers["authorization"] = "Bearer ";
    EXPECT_EQ(h.handle(r).status, 401);
    r.headers["authorization"] = "Bearer x";
    EXPECT_EQ(h.handle(r).status, 401);
}

// ---- Host / Origin ---------------------------------------------------------------------------
TEST(ApiHostOrigin, DnsRebindingHostRejectedEvenWithValidToken) {
    Api a;
    for (const char* h : {"evil.example.com", "evil.example.com:8765", "127.0.0.1.evil.com", "localhost.evil.com", "", "10.1.2.3:8765"}) {
        auto q = a.req("GET", "/v1/models");
        q.headers["host"] = h;
        auto r = a.handler.handle(q);
        EXPECT_EQ(r.status, 403) << h;
        EXPECT_EQ(err_of(r)["code"], "host_not_allowed") << h;
    }
    auto q = a.req("GET", "/v1/models");
    q.headers.erase("host");
    EXPECT_EQ(a.handler.handle(q).status, 403);
}

TEST(ApiHostOrigin, LoopbackHostsAccepted) {
    Api a;
    for (const char* h : {"127.0.0.1", "127.0.0.1:8765", "localhost:8765", "LOCALHOST", "[::1]:8765"}) {
        auto q = a.req("GET", "/v1/models");
        q.headers["host"] = h;
        EXPECT_EQ(a.handler.handle(q).status, 200) << h;
    }
}

TEST(ApiHostOrigin, LanModeAllowsIpLiteralsButNotNames) {
    ApiOptions o;
    o.lan = true;
    o.bind_host = "0.0.0.0";
    Api a(o);
    auto q = a.req("GET", "/v1/models");
    q.headers["host"] = "192.168.1.20:8765";
    EXPECT_EQ(a.handler.handle(q).status, 200);
    q.headers["host"] = "attacker.example:8765";
    EXPECT_EQ(a.handler.handle(q).status, 403);
    // explicit allow-list
    ApiOptions o2;
    o2.lan = true;
    o2.allowed_hosts = {"mybox.lan"};
    Api b(o2);
    auto q2 = b.req("GET", "/v1/models");
    q2.headers["host"] = "mybox.lan:8765";
    EXPECT_EQ(b.handler.handle(q2).status, 200);
}

TEST(ApiHostOrigin, ForeignOriginsRejected) {
    Api a;
    for (const char* o : {"http://evil.example", "https://evil.example:8765", "null", "file://", "http://127.0.0.1:9999",
                          "http://localhost.evil.com:8765", "http://127.0.0.1:8765/path", "ftp://127.0.0.1:8765", ""}) {
        auto q = a.req("GET", "/v1/models");
        q.headers["origin"] = o;
        auto r = a.handler.handle(q);
        EXPECT_EQ(r.status, 403) << o;
        EXPECT_EQ(err_of(r)["code"], "origin_not_allowed") << o;
    }
}

TEST(ApiHostOrigin, SameOriginAndAllowListAccepted) {
    ApiOptions o;
    o.allowed_origins = {"http://127.0.0.1:3000"};
    Api a(o);
    for (const char* org : {"http://127.0.0.1:8765", "http://localhost:8765", "http://[::1]:8765", "http://127.0.0.1:3000"}) {
        auto q = a.req("GET", "/v1/models");
        q.headers["origin"] = org;
        EXPECT_EQ(a.handler.handle(q).status, 200) << org;
    }
}

TEST(ApiHostOrigin, NoCorsHeadersAreEverEmitted) {
    Api a;
    auto q = a.req("GET", "/v1/models");
    q.headers["origin"] = "http://127.0.0.1:8765";
    auto r = a.handler.handle(q);
    for (auto& [k, v] : r.headers) EXPECT_NE(k.rfind("Access-Control", 0), 0u) << k;
}

// ---- request validation ------------------------------------------------------------------------
TEST(ApiValidation, OversizeBodyIs413) {
    ApiOptions o;
    o.max_body_bytes = 128;
    Api a(o);
    auto r = a.call("POST", "/v1/chat/completions", a.chat_body(std::string(512, 'x')).dump());
    EXPECT_EQ(r.status, 413);
    EXPECT_EQ(err_of(r)["code"], "payload_too_large");
}

TEST(ApiValidation, MalformedJsonIs400) {
    Api a;
    for (const char* bad : {"{", "not json", "{\"messages\":[}", "[1,2", "{\"a\":}"}) {
        auto r = a.call("POST", "/v1/chat/completions", bad);
        EXPECT_EQ(r.status, 400) << bad;
        EXPECT_EQ(err_of(r)["code"], "invalid_json") << bad;
    }
}

TEST(ApiValidation, PathologicalNestingIs400) {
    Api a;
    std::string deep(200, '[');
    deep += std::string(200, ']');
    auto r = a.call("POST", "/v1/chat/completions", deep);
    EXPECT_EQ(r.status, 400);
}

TEST(ApiValidation, SchemaViolationsAre400) {
    Api a;
    for (const char* bad : {
             R"({})", R"({"messages":[]})", R"({"messages":"hi"})",
             R"({"messages":[{"role":"user"}]})", R"({"messages":[{"role":"root","content":"x"}]})",
             R"({"messages":[{"role":"user","content":"x"}],"max_tokens":0})",
             R"({"messages":[{"role":"user","content":"x"}],"max_tokens":999999})",
             R"({"messages":[{"role":"user","content":"x"}],"temperature":5})",
             R"({"messages":[{"role":"user","content":"x"}],"stream":"yes"})",
             R"({"messages":[{"role":"user","content":"x"}],"n":2})",
             R"({"messages":[{"role":"user","content":"x"}],"model":"../../etc/passwd"})"}) {
        auto r = a.call("POST", "/v1/chat/completions", bad);
        EXPECT_EQ(r.status, 400) << bad << " -> " << r.body;
    }
}

TEST(ApiValidation, WrongContentTypeIs415) {
    Api a;
    auto q = a.req("POST", "/v1/chat/completions", a.chat_body().dump());
    q.headers["content-type"] = "text/plain";
    EXPECT_EQ(a.handler.handle(q).status, 415);
    q.headers.erase("content-type");
    EXPECT_EQ(a.handler.handle(q).status, 415);
}

TEST(ApiValidation, UnsupportedFeaturesAre501NotSilentlyIgnored) {
    Api a;
    json b = a.chat_body();
    b["tools"] = json::array();
    EXPECT_EQ(a.call("POST", "/v1/chat/completions", b.dump()).status, 501);
    // Image input is implemented (tests/contract/api_vision); the OpenAI "input_image" shape is not, and
    // a data URI that is not a real image is a 400, never a silent ignore.
    json c = a.chat_body();
    c["messages"][0]["content"] = json::array({json{{"type", "input_image"}, {"image_url", "x"}}});
    EXPECT_EQ(a.call("POST", "/v1/chat/completions", c.dump()).status, 400);
    json d = a.chat_body();
    d["messages"][0]["content"] = json::array({json{{"type", "text"}, {"text", "what is this"}},
                                               json{{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,AAAA"}}}}});
    EXPECT_EQ(a.call("POST", "/v1/chat/completions", d.dump()).status, 400);
}

TEST(ApiRouting, UnknownRoutesAndMethods) {
    Api a;
    EXPECT_EQ(a.call("GET", "/nope").status, 404);
    EXPECT_EQ(a.call("GET", "/v1/models/").status, 404);
    EXPECT_EQ(a.call("GET", "//v1/models").status, 404);
    auto r = a.call("POST", "/v1/models", "{}");
    EXPECT_EQ(r.status, 405);
    EXPECT_EQ(a.call("GET", "/v1/chat/completions").status, 405);
    EXPECT_EQ(a.call("OPTIONS", "/v1/models").status, 405);
    EXPECT_EQ(a.call("DELETE", "/hcs/v1/models/tiny/load").status, 405);
}

TEST(ApiRouting, RequestIdOnEverythingAndUnique) {
    Api a;
    auto rid = [](const HttpResponse& r) {
        for (auto& [k, v] : r.headers) if (k == "X-Request-Id") return v;
        return std::string();
    };
    auto r1 = a.call("GET", "/v1/models");
    auto r2 = a.call("GET", "/v1/models", {}, false);
    auto r3 = a.call("GET", "/nope");
    EXPECT_FALSE(rid(r1).empty());
    EXPECT_NE(rid(r1), rid(r2));
    EXPECT_NE(rid(r2), rid(r3));
    EXPECT_EQ(err_of(r2)["request_id"], rid(r2));  // structured error carries the same id
}

// ---- native routes -------------------------------------------------------------------------------
TEST(ApiNative, HealthRuntimeDevicesMemoryModels) {
    Api a;
    auto h = json::parse(a.call("GET", "/hcs/v1/health", {}, false).body);
    EXPECT_EQ(h["status"], "ok");
    auto rt = json::parse(a.call("GET", "/hcs/v1/runtime").body);
    EXPECT_TRUE(rt["text_engine"]["available"].get<bool>());
    EXPECT_FALSE(rt["image_engine"]["available"].get<bool>());
    auto dv = a.call("GET", "/hcs/v1/devices");
    EXPECT_EQ(dv.status, 200);
    EXPECT_TRUE(json::parse(dv.body).is_object());
    auto mem = json::parse(a.call("GET", "/hcs/v1/memory").body);
    EXPECT_FALSE(mem["budget_known"].get<bool>());
    EXPECT_EQ(mem["total_bytes"], "unknown");  // unknown is reported as unknown, not guessed
    auto ms = json::parse(a.call("GET", "/hcs/v1/models").body);
    EXPECT_EQ(ms["models"].size(), 2u);
    for (auto& m : ms["models"]) EXPECT_FALSE(m.contains("path")) << "file paths must not leak";
    auto one = a.call("GET", "/hcs/v1/models/tiny");
    EXPECT_EQ(one.status, 200);
    EXPECT_EQ(a.call("GET", "/hcs/v1/models/missing").status, 404);
    auto oa = json::parse(a.call("GET", "/v1/models").body);
    EXPECT_EQ(oa["object"], "list");
    EXPECT_EQ(oa["data"].size(), 2u);
}

TEST(ApiNative, ImageRoutesReturn501WithReason) {
    Api a;
    for (const char* p : {"/v1/images/generations", "/v1/images/edits"}) {
        auto r = a.call("POST", p, R"({"prompt":"x"})");
        EXPECT_EQ(r.status, 501) << p;
        EXPECT_EQ(err_of(r)["code"], "unsupported");
        EXPECT_NE(err_of(r)["message"].get<std::string>().find("image engine"), std::string::npos);
    }
}

TEST(ApiNative, ModelLoadIsAJobAndUnloadWorks) {
    Api a;
    auto r = a.call("POST", "/hcs/v1/models/tiny/load");
    ASSERT_EQ(r.status, 202) << r.body;
    const std::string job = json::parse(r.body)["job_id"];
    auto id = ControlPlane::parse_job_id(job);
    ASSERT_TRUE(id);
    ASSERT_TRUE(a.fx.cp->wait_job(*id, std::chrono::seconds(5)));
    auto st = json::parse(a.call("GET", "/hcs/v1/jobs/" + job).body);
    EXPECT_EQ(st["state"], "succeeded");
    EXPECT_EQ(json::parse(a.call("GET", "/hcs/v1/models/tiny").body)["state"], "loaded");
    EXPECT_EQ(a.fx.engine.loads.load(), 1);
    EXPECT_EQ(a.call("POST", "/hcs/v1/models/other/unload").status, 409);  // not the loaded one
    EXPECT_EQ(a.call("POST", "/hcs/v1/models/tiny/unload").status, 200);
    EXPECT_EQ(a.fx.engine.unloads.load(), 1);
    EXPECT_EQ(a.call("POST", "/hcs/v1/models/tiny/unload").status, 409);
}

TEST(ApiNative, LoadPathTraversalInModelIdIsRejected) {
    Api a;
    for (const char* id : {"..", "..%2f..%2fetc", "....//", "a/../b", "C:%5Cwindows", "tiny%00", "TINY", ".hidden"}) {
        auto r = a.call("POST", std::string("/hcs/v1/models/") + id + "/load");
        EXPECT_TRUE(r.status == 400 || r.status == 404) << id << " -> " << r.status;
    }
    EXPECT_EQ(a.fx.engine.loads.load(), 0);
}

TEST(ApiNative, RegistryPathOutsideApprovedRootNeverReachesEngine) {
    Api a;
    test::TempDir outside;
    test::write_file(outside / "evil.gguf", test::fake_gguf());
    ModelManifest m;
    m.id = "evil";
    m.display_name = "evil";
    m.path = (outside / "evil.gguf").string();  // e.g. a hand-edited registry.json
    m.format = ModelFormat::Gguf;
    m.kind = ModelKind::Text;
    m.size_bytes = 68;
    ASSERT_TRUE(a.fx.registry.add(m));
    auto r = a.call("POST", "/hcs/v1/models/evil/load");
    ASSERT_EQ(r.status, 202);
    auto id = ControlPlane::parse_job_id(json::parse(r.body)["job_id"]);
    ASSERT_TRUE(a.fx.cp->wait_job(*id, std::chrono::seconds(5)));
    auto st = json::parse(a.call("GET", "/hcs/v1/jobs/" + json::parse(r.body)["job_id"].get<std::string>()).body);
    EXPECT_EQ(st["state"], "failed");
    EXPECT_EQ(st["error"]["code"], "security_violation");
    EXPECT_EQ(a.fx.engine.loads.load(), 0);
}

TEST(ApiNative, MalformedJobIds) {
    Api a;
    for (const char* id : {"1", "job-", "job-0", "job-abc", "job--1", "job-99999999999999999999999", "JOB-1"})
        EXPECT_EQ(a.call("GET", std::string("/hcs/v1/jobs/") + id).status, 400) << id;
    EXPECT_EQ(a.call("GET", "/hcs/v1/jobs/job-424242").status, 404);
}

// ---- chat -----------------------------------------------------------------------------------------
TEST(ApiChat, NonStreamingCompletionAutoLoadsModel) {
    Api a;
    auto r = a.call("POST", "/v1/chat/completions", a.chat_body("say hi").dump());
    ASSERT_EQ(r.status, 200) << r.body;
    auto j = json::parse(r.body);
    EXPECT_EQ(j["object"], "chat.completion");
    EXPECT_EQ(j["model"], "tiny");
    EXPECT_EQ(j["choices"][0]["message"]["content"], "Hello from the fake engine");
    EXPECT_EQ(j["choices"][0]["finish_reason"], "stop");
    EXPECT_EQ(j["usage"]["completion_tokens"], 5);
    EXPECT_EQ(a.fx.engine.loads.load(), 1);
    // The engine only ever saw a path inside the approved root.
    EXPECT_EQ(std::filesystem::path(a.fx.engine.last_path).parent_path(), a.fx.root);
    // second call reuses the loaded model
    EXPECT_EQ(a.call("POST", "/v1/chat/completions", a.chat_body("again").dump()).status, 200);
    EXPECT_EQ(a.fx.engine.loads.load(), 1);
}

TEST(ApiChat, NoModelLoadedAndNoModelGivenIs409) {
    Api a;
    json b = {{"messages", json::array({json{{"role", "user"}, {"content", "x"}}})}};
    auto r = a.call("POST", "/v1/chat/completions", b.dump());
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(err_of(r)["code"], "invalid_state");
}

TEST(ApiChat, UnknownModelIs404) {
    Api a;
    auto r = a.call("POST", "/v1/chat/completions", a.chat_body("x", false, "nonexistent").dump());
    EXPECT_EQ(r.status, 404);
}

TEST(ApiChat, TemplateRouting) {
    Api a;
    json single = {{"model", "tiny"},
                   {"messages", json::array({json{{"role", "system"}, {"content", "be brief"}}, json{{"role", "user"}, {"content", "hello"}}})}};
    ASSERT_EQ(a.call("POST", "/v1/chat/completions", single.dump()).status, 200);
    EXPECT_TRUE(a.fx.engine.last_apply_template);
    EXPECT_EQ(a.fx.engine.last_prompt, "hello");
    EXPECT_EQ(a.fx.engine.last_system, "be brief");
    json multi = {{"model", "tiny"},
                  {"messages", json::array({json{{"role", "user"}, {"content", "a"}}, json{{"role", "assistant"}, {"content", "b"}},
                                            json{{"role", "user"}, {"content", "c"}}})}};
    ASSERT_EQ(a.call("POST", "/v1/chat/completions", multi.dump()).status, 200);
    EXPECT_FALSE(a.fx.engine.last_apply_template);
    EXPECT_NE(a.fx.engine.last_prompt.find("<|im_start|>assistant\nb<|im_end|>"), std::string::npos);
    EXPECT_NE(a.fx.engine.last_prompt.rfind("<|im_start|>assistant\n"), std::string::npos);
}

TEST(ApiChat, SseStreaming) {
    Api a;
    auto r = a.call("POST", "/v1/chat/completions", a.chat_body("stream please", true).dump());
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(r.content_type, "text/event-stream");
    ASSERT_TRUE(static_cast<bool>(r.stream));
    std::vector<std::string> chunks;
    r.stream([&](std::string_view c) {
        chunks.emplace_back(c);
        return true;
    });
    ASSERT_GE(chunks.size(), 4u);
    std::string text;
    bool saw_role = false, saw_finish = false;
    for (const auto& c : chunks) {
        ASSERT_EQ(c.rfind("data: ", 0), 0u) << c;
        ASSERT_EQ(c.substr(c.size() - 2), "\n\n");
        const std::string payload = c.substr(6, c.size() - 8);
        if (payload == "[DONE]") continue;
        auto j = json::parse(payload);
        EXPECT_EQ(j["object"], "chat.completion.chunk");
        const auto& d = j["choices"][0]["delta"];
        if (d.contains("role")) saw_role = true;
        if (d.contains("content")) text += d["content"].get<std::string>();
        if (!j["choices"][0]["finish_reason"].is_null()) { saw_finish = true; EXPECT_EQ(j["choices"][0]["finish_reason"], "stop"); }
    }
    EXPECT_TRUE(saw_role);
    EXPECT_TRUE(saw_finish);
    EXPECT_EQ(text, "Hello from the fake engine");
    EXPECT_EQ(chunks.back(), "data: [DONE]\n\n");
}

TEST(ApiChat, SseClientDisconnectCancelsTheRealWork) {
    Api a;
    a.fx.engine.run_until_cancel = true;
    a.fx.engine.delay_ms = 2;
    auto r = a.call("POST", "/v1/chat/completions", a.chat_body("x", true).dump());
    ASSERT_EQ(r.status, 200);
    int n = 0;
    r.stream([&](std::string_view) { return ++n < 4; });  // "client" goes away after 3 writes
    ASSERT_TRUE(wait_for([&] { return !a.fx.engine.generating.load(); })) << "engine kept generating after disconnect";
    auto st = a.fx.cp->job_status(JobId{1});
    ASSERT_TRUE(st);
    ASSERT_TRUE(a.fx.cp->wait_job(JobId{1}, std::chrono::seconds(3)));
    EXPECT_EQ(a.fx.cp->job_status(JobId{1}).value().state, JobState::Cancelled);
}

TEST(ApiChat, CancelEndpointStopsRunningGeneration) {
    Api a;
    a.fx.engine.run_until_cancel = true;
    a.fx.engine.delay_ms = 2;
    HttpResponse resp;
    std::thread t([&] { resp = a.call("POST", "/v1/chat/completions", a.chat_body("x").dump()); });
    ASSERT_TRUE(wait_for([&] { return a.fx.engine.generating.load(); }));
    auto c = a.call("POST", "/hcs/v1/jobs/job-1/cancel");
    EXPECT_EQ(c.status, 200);
    t.join();
    EXPECT_EQ(resp.status, 409);
    EXPECT_EQ(err_of(resp)["code"], "cancelled");
    EXPECT_FALSE(a.fx.engine.generating.load());
    ASSERT_TRUE(a.fx.cp->wait_job(JobId{1}, std::chrono::seconds(3)));
    EXPECT_EQ(json::parse(a.call("GET", "/hcs/v1/jobs/job-1").body)["state"], "cancelled");
    // The engine is usable again afterwards.
    a.fx.engine.run_until_cancel = false;
    EXPECT_EQ(a.call("POST", "/v1/chat/completions", a.chat_body("again").dump()).status, 200);
}

TEST(ApiChat, RequestTimeoutCancelsAndReturns504) {
    ApiOptions o;
    o.request_timeout = std::chrono::milliseconds(150);
    Api a(o);
    a.fx.engine.run_until_cancel = true;
    a.fx.engine.delay_ms = 2;
    auto r = a.call("POST", "/v1/chat/completions", a.chat_body("x").dump());
    EXPECT_EQ(r.status, 504);
    EXPECT_TRUE(wait_for([&] { return !a.fx.engine.generating.load(); }));
}

TEST(ApiChat, ConcurrencyBoundReturns429) {
    ApiOptions o;
    o.max_inflight = 1;
    Api a(o);
    a.fx.engine.run_until_cancel = true;
    a.fx.engine.delay_ms = 2;
    HttpResponse resp;
    std::thread t([&] { resp = a.call("POST", "/v1/chat/completions", a.chat_body("x").dump()); });
    ASSERT_TRUE(wait_for([&] { return a.fx.engine.generating.load(); }));
    auto r = a.call("GET", "/v1/models");
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(a.call("GET", "/hcs/v1/health", {}, false).status, 200);  // health is never throttled
    EXPECT_EQ(a.call("POST", "/hcs/v1/jobs/job-1/cancel").status, 200);  // cancel is never throttled
    t.join();
    EXPECT_EQ(a.call("GET", "/v1/models").status, 200);  // slot released
}

TEST(ApiChat, NoEngineReportsUnsupportedHonestly) {
    Api a({}, /*with_engine=*/false);
    auto r = a.call("POST", "/v1/chat/completions", a.chat_body().dump());
    EXPECT_EQ(r.status, 501);
    EXPECT_EQ(a.call("POST", "/hcs/v1/models/tiny/load").status, 501);
    EXPECT_FALSE(json::parse(a.call("GET", "/hcs/v1/runtime").body)["text_engine"]["available"].get<bool>());
}

TEST(ApiChat, LoadFailureSurfacesAsErrorAndModelIsRecoverable) {
    Api a;
    a.fx.engine.fail_load = true;
    auto r = a.call("POST", "/v1/chat/completions", a.chat_body().dump());
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(json::parse(a.call("GET", "/hcs/v1/models/tiny").body)["state"], "failed");
    a.fx.engine.fail_load = false;
    EXPECT_EQ(a.call("POST", "/v1/chat/completions", a.chat_body().dump()).status, 200);  // retry succeeds
}

TEST(ApiPrivacy, PromptsAreNeverLogged) {
    Api a;
    const std::string secret = "TOP-SECRET-PROMPT-CONTENT-9137";
    ASSERT_EQ(a.call("POST", "/v1/chat/completions", a.chat_body(secret).dump()).status, 200);
    ASSERT_EQ(a.call("POST", "/v1/chat/completions", a.chat_body(secret, true).dump()).status, 200);
    EXPECT_EQ(a.fx.log_sink.str().find(secret), std::string::npos);
    EXPECT_EQ(a.fx.log_sink.str().find(kToken), std::string::npos) << "bearer token must not be logged";
    EXPECT_EQ(a.fx.log_sink.str().find("Hello from"), std::string::npos) << "completions must not be logged";
}
