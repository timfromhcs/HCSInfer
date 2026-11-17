// Contract tests for /v1/images/{generations,edits} and the embedded UI hook. FakeImageEngine exercises the
// plumbing only (validation, job, cancellation, PNG transport, lifecycle); it is NOT evidence of image
// generation (AGENTS.md 16.2). Real stable-diffusion.cpp output is covered by tests/e2e/image_real.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "hcs/api/http_api.hpp"
#include "image_api_fixture.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;

namespace {

const char* kToken = "test-token-0123456789abcdef";

struct Api {
    test::ImageApiFixture fx;
    ApiHandler handler;
    explicit Api(ApiOptions o = {}, bool with_image = true, ControlPlaneOptions cpo = {})
        : fx(with_image, cpo), handler(*fx.cp, finish(std::move(o)), fx.logger) {}

    static ApiOptions finish(ApiOptions o) {
        if (o.token.empty())
            o.token = kToken;
        if (o.port == 0)
            o.port = 8765;
        return o;
    }
    HttpRequest req(const std::string& method, const std::string& path, const std::string& body = {},
                    bool auth = true) {
        HttpRequest r;
        r.method = method;
        r.path = path;
        r.body = body;
        r.headers["host"] = "127.0.0.1:8765";
        if (auth)
            r.headers["authorization"] = std::string("Bearer ") + kToken;
        if (!body.empty())
            r.headers["content-type"] = "application/json";
        return r;
    }
    HttpResponse call(const std::string& method, const std::string& path, const json& body,
                      bool auth = true) {
        return handler.handle(req(method, path, body.dump(), auth));
    }
    HttpResponse call(const std::string& method, const std::string& path, bool auth = true) {
        return handler.handle(req(method, path, {}, auth));
    }
    json gen_body() const {
        return {
            {"model", "sdmodel"}, {"prompt", "a red apple"}, {"size", "256x256"}, {"steps", 2}, {"seed", 5}};
    }
};

json err_of(const HttpResponse& r) {
    return json::parse(r.body)["error"];
}

bool wait_for(const std::function<bool()>& f, int ms = 3000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

}  // namespace

// ---- generations ---------------------------------------------------------------------------------
TEST(ApiImages, GenerationReturnsBase64PngAndRecordsTheJob) {
    Api a;
    auto r = a.call("POST", "/v1/images/generations", a.gen_body());
    ASSERT_EQ(r.status, 200) << r.body;
    const json j = json::parse(r.body);
    ASSERT_EQ(j["data"].size(), 1u);
    EXPECT_EQ(j["data"][0]["b64_json"].get<std::string>().rfind("iVBORw0KGgo", 0), 0u)
        << "PNG signature in base64";
    EXPECT_EQ(j["hcs"]["width"], 256);
    EXPECT_EQ(j["hcs"]["steps"], 2);
    EXPECT_EQ(j["hcs"]["model"], "sdmodel");
    EXPECT_EQ(j["hcs"]["seed"], 5);
    EXPECT_EQ(a.fx.image.generations.load(), 1);
    EXPECT_EQ(a.fx.image.loads.load(), 1);
    EXPECT_EQ(a.fx.image.last.width, 256u);
    EXPECT_EQ(a.fx.image.last.height, 256u);
    EXPECT_EQ(a.fx.image.last.steps, 2u);
    ASSERT_TRUE(a.fx.image.last.seed.has_value());
    EXPECT_EQ(*a.fx.image.last.seed, 5u);
    // the job is visible through the native API and summarises the image without embedding it
    auto st = json::parse(a.call("GET", "/hcs/v1/jobs/" + j["hcs"]["job_id"].get<std::string>()).body);
    EXPECT_EQ(st["state"], "succeeded");
    EXPECT_EQ(st["result"]["kind"], "image");
    EXPECT_EQ(st["result"]["width"], 256);
    EXPECT_FALSE(st["result"].contains("b64_json"));
}

TEST(ApiImages, DefaultsApplyWhenOnlyAPromptIsGiven) {
    Api a;
    ASSERT_EQ(a.call("POST", "/hcs/v1/models/sdmodel/load").status, 202);
    ASSERT_TRUE(wait_for([&] { return a.fx.image.loads.load() == 1; }));
    auto r = a.call("POST", "/v1/images/generations", json{{"prompt", "x"}, {"size", "64x64"}, {"steps", 1}});
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(a.fx.image.loads.load(), 1) << "already loaded: not reloaded";
}

TEST(ApiImages, WrongMethodAndMissingEngine) {
    Api a;
    auto g = a.call("GET", "/v1/images/generations");
    EXPECT_EQ(g.status, 405);
    Api none({}, /*with_image=*/false);
    auto r = none.call("POST", "/v1/images/generations", json{{"prompt", "x"}});
    EXPECT_EQ(r.status, 501);
    EXPECT_EQ(err_of(r)["code"], "unsupported");
    EXPECT_NE(err_of(r)["message"].get<std::string>().find("image engine"), std::string::npos);
    EXPECT_EQ(none.call("POST", "/v1/images/edits", json{{"prompt", "x"}}).status, 501);
}

TEST(ApiImages, RejectsInvalidRequestsBeforeTheEngineIsTouched) {
    Api a;
    struct Case {
        json patch;
        int status;
        const char* what;
    };
    const std::vector<Case> cases = {
        {{{"size", "250x256"}}, 400, "not a multiple of 8"},
        {{{"size", "32x32"}}, 400, "below 64"},
        {{{"size", "4096x4096"}}, 400, "above max dimension"},
        {{{"size", "1536x1536"}}, 400, "pixel budget"},
        {{{"size", "256"}}, 400, "malformed size"},
        {{{"size", "-256x256"}}, 400, "negative size"},
        {{{"size", "256x256x3"}}, 400, "malformed size 3"},
        {{{"steps", 0}}, 400, "steps 0"},
        {{{"steps", 151}}, 400, "steps over cap"},
        {{{"steps", "two"}}, 400, "steps type"},
        {{{"n", 2}}, 400, "n != 1"},
        {{{"cfg_scale", 31}}, 400, "cfg too big"},
        {{{"seed", -1}}, 400, "negative seed"},
        {{{"prompt", ""}}, 400, "empty prompt"},
        {{{"model", "tiny"}}, 400, "text model is not an image model"},
        {{{"model", "no-such-model"}}, 404, "unknown model"},
        {{{"model", "../etc/passwd"}}, 400, "bad model id"},
        {{{"response_format", "url"}}, 501, "url responses"},
        {{{"mask", "x"}}, 501, "inpainting"},
    };
    for (const auto& c : cases) {
        json b = a.gen_body();
        for (auto it = c.patch.begin(); it != c.patch.end(); ++it) b[it.key()] = it.value();
        auto r = a.call("POST", "/v1/images/generations", b);
        EXPECT_EQ(r.status, c.status) << c.what << ": " << r.body;
    }
    json no_prompt = a.gen_body();
    no_prompt.erase("prompt");
    EXPECT_EQ(a.call("POST", "/v1/images/generations", no_prompt).status, 400);
    EXPECT_EQ(a.fx.image.generations.load(), 0);
    EXPECT_EQ(a.fx.image.loads.load(), 0);
}

TEST(ApiImages, MalformedOversizedAndWrongContentType) {
    ApiOptions o;
    o.max_body_bytes = 4096;
    Api a(o);
    auto bad = a.req("POST", "/v1/images/generations", "{not json");
    EXPECT_EQ(a.handler.handle(bad).status, 400);
    auto big = a.req("POST", "/v1/images/generations", std::string(8192, ' '));
    EXPECT_EQ(a.handler.handle(big).status, 413);
    auto wrong = a.req("POST", "/v1/images/generations", a.gen_body().dump());
    wrong.headers["content-type"] = "text/plain";
    EXPECT_EQ(a.handler.handle(wrong).status, 415);
    auto deep = a.req("POST", "/v1/images/generations", std::string(2000, '[') + std::string(2000, ']'));
    EXPECT_EQ(a.handler.handle(deep).status, 400);
    EXPECT_EQ(a.fx.image.generations.load(), 0);
}

TEST(ApiImages, NoImageModelLoadedIsAConflictNotACrash) {
    Api a;
    auto r = a.call("POST", "/v1/images/generations", json{{"prompt", "x"}, {"size", "64x64"}, {"steps", 1}});
    EXPECT_EQ(r.status, 409) << r.body;
    EXPECT_EQ(err_of(r)["code"], "invalid_state");
}

TEST(ApiImages, SwitchingBetweenTextAndImageModelsUnloadsThePreviousOne) {
    Api a;
    ASSERT_EQ(a.call("POST", "/v1/images/generations", a.gen_body()).status, 200);
    EXPECT_EQ(a.fx.image.loads.load(), 1);
    json chat = {{"model", "tiny"}, {"messages", json::array({json{{"role", "user"}, {"content", "hi"}}})}};
    ASSERT_EQ(a.call("POST", "/v1/chat/completions", chat).status, 200);
    EXPECT_EQ(a.fx.image.unloads.load(), 1) << "image model released when the text model loads";
    EXPECT_EQ(a.fx.text.loads.load(), 1);
    ASSERT_EQ(a.call("POST", "/v1/images/generations", a.gen_body()).status, 200);
    EXPECT_EQ(a.fx.text.unloads.load(), 1);
    EXPECT_EQ(a.fx.image.loads.load(), 2);
}

TEST(ApiImages, ClientDisconnectCancelsTheRender) {
    Api a;
    a.fx.image.run_until_cancel = true;
    std::atomic<bool> gone{false};
    auto q = a.req("POST", "/v1/images/generations", a.gen_body().dump());
    q.peer_closed = [&] { return gone.load(); };
    HttpResponse r;
    std::thread t([&] { r = a.handler.handle(q); });
    ASSERT_TRUE(wait_for([&] { return a.fx.image.rendering.load(); }));
    gone = true;
    t.join();
    EXPECT_EQ(r.status, 499);
    EXPECT_TRUE(wait_for([&] { return !a.fx.image.rendering.load(); })) << "the engine stopped rendering";
}

TEST(ApiImages, RequestTimeoutCancelsTheRenderAndReturns504) {
    ApiOptions o;
    o.request_timeout = std::chrono::milliseconds(300);
    Api a(o);
    a.fx.image.run_until_cancel = true;
    auto r = a.call("POST", "/v1/images/generations", a.gen_body());
    EXPECT_EQ(r.status, 504) << r.body;
    EXPECT_TRUE(wait_for([&] { return !a.fx.image.rendering.load(); }));
}

TEST(ApiImages, JobCancelEndpointStopsAnImageJob) {
    Api a;
    a.fx.image.run_until_cancel = true;
    // Submit through the control plane (the route returns no job id before completion) and cancel via the
    // API.
    ImageParams p;
    p.model = "sdmodel";
    p.prompt = "x";
    p.width = p.height = 64;
    p.steps = 1;
    auto sub = a.fx.cp->submit_image(p, false);
    ASSERT_TRUE(sub);
    ASSERT_TRUE(wait_for([&] { return a.fx.image.rendering.load(); }));
    auto c = a.call("POST", "/hcs/v1/jobs/" + to_string(sub.value().id) + "/cancel");
    EXPECT_EQ(c.status, 200) << c.body;
    ASSERT_TRUE(a.fx.cp->wait_job(sub.value().id, std::chrono::seconds(3)));
    auto st = json::parse(a.call("GET", "/hcs/v1/jobs/" + to_string(sub.value().id)).body);
    EXPECT_EQ(st["state"], "cancelled");
    EXPECT_FALSE(a.fx.image.rendering.load());
}

TEST(ApiImages, EngineFailureBecomesAStructuredError) {
    Api a;
    a.fx.image.fail_load = true;
    auto r = a.call("POST", "/v1/images/generations", a.gen_body());
    EXPECT_EQ(r.status, 422) << r.body;
    EXPECT_EQ(err_of(r)["code"], "model_error");
}

// ---- edits ---------------------------------------------------------------------------------------
TEST(ApiImageEdits, AcceptsADataUriAndKeepsTheInputSizeByDefault) {
    Api a;
    const auto png = test::make_red_square_png(64);
    json b = {{"model", "sdmodel"},
              {"prompt", "make it blue"},
              {"image", test::png_data_uri(png)},
              {"steps", 2},
              {"strength", 0.5}};
    auto r = a.call("POST", "/v1/images/edits", b);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(a.fx.image.edits.load(), 1);
    EXPECT_EQ(a.fx.image.generations.load(), 0);
    EXPECT_EQ(a.fx.image.last.width, 64u);
    EXPECT_EQ(a.fx.image.last.height, 64u);
    ASSERT_TRUE(a.fx.image.last.init_image.has_value());
    EXPECT_EQ(a.fx.image.last.init_image->bytes, png);
    EXPECT_FLOAT_EQ(a.fx.image.last.strength, 0.5f);
}

TEST(ApiImageEdits, BareBase64IsAccepted) {
    Api a;
    const auto png = test::make_red_square_png(64);
    auto r = a.call("POST", "/v1/images/edits",
                    json{{"model", "sdmodel"}, {"prompt", "x"}, {"image", test::base64(png)}, {"steps", 1}});
    EXPECT_EQ(r.status, 200) << r.body;
}

TEST(ApiImageEdits, RejectsBadInputImagesAndMultipart) {
    Api a;
    const auto png = test::make_red_square_png(64);
    auto base = [&](json patch) {
        json b = {{"model", "sdmodel"}, {"prompt", "x"}, {"image", test::png_data_uri(png)}, {"steps", 1}};
        for (auto it = patch.begin(); it != patch.end(); ++it) b[it.key()] = it.value();
        return a.call("POST", "/v1/images/edits", b);
    };
    EXPECT_EQ(base({{"image", "data:image/png;base64,AAAA"}}).status, 400) << "not an image";
    EXPECT_EQ(base({{"image", "data:image/png;base64,@@@@"}}).status, 400) << "bad base64";
    EXPECT_EQ(base({{"image", "http://example.com/a.png"}}).status, 400) << "remote URL";
    EXPECT_EQ(base({{"image", "file:///c:/windows/win.ini"}}).status, 400) << "file URL";
    EXPECT_EQ(base({{"image", test::png_data_uri(test::make_png_header_only(20000, 20000))}}).status, 400)
        << "pixel limit";
    EXPECT_EQ(base({{"image", 5}}).status, 400) << "image must be a string";
    EXPECT_EQ(base({{"mask", "x"}}).status, 501);
    json missing = {{"model", "sdmodel"}, {"prompt", "x"}};
    EXPECT_EQ(a.call("POST", "/v1/images/edits", missing).status, 400) << "image is required";
    auto mp = a.req("POST", "/v1/images/edits", "--b\r\n");
    mp.headers["content-type"] = "multipart/form-data; boundary=b";
    auto r = a.handler.handle(mp);
    EXPECT_EQ(r.status, 415);
    EXPECT_NE(err_of(r)["message"].get<std::string>().find("application/json"), std::string::npos);
    EXPECT_EQ(a.fx.image.edits.load(), 0);
}

// ---- jobs / runtime reflect the image engine ---------------------------------------------------------
TEST(ApiImages, RuntimeReportsTheImageEngine) {
    Api a;
    auto rt = json::parse(a.call("GET", "/hcs/v1/runtime").body);
    EXPECT_TRUE(rt["image_engine"]["available"].get<bool>());
    EXPECT_TRUE(rt["image_engine"]["streaming"].get<bool>());
}

// ---- embedded UI hook ------------------------------------------------------------------------------
#ifdef HCS_TEST_GUI
TEST(ApiUi, RootAndAssetsAreServedWithoutATokenButBehindHostAndOriginChecks) {
    Api a;
    auto r = a.call("GET", "/", false);
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.content_type.find("text/html"), std::string::npos);
    EXPECT_FALSE(r.body.empty());
    bool csp = false, nosniff = false;
    for (auto& [k, v] : r.headers) {
        if (k == "Content-Security-Policy")
            csp = v.find("default-src 'none'") != std::string::npos;
        if (k == "X-Content-Type-Options")
            nosniff = v == "nosniff";
    }
    EXPECT_TRUE(csp);
    EXPECT_TRUE(nosniff);
    EXPECT_EQ(a.call("GET", "/ui/app.js", false).status, 200);
    EXPECT_EQ(a.call("GET", "/ui/app.css", false).status, 200);
    EXPECT_EQ(a.call("GET", "/ui/nope.js", false).status, 404);
    EXPECT_EQ(a.call("GET", "/ui/..%2fsecret", false).status, 400);

    auto evil = a.req("GET", "/");
    evil.headers["host"] = "evil.example.com";
    EXPECT_EQ(a.handler.handle(evil).status, 403) << "DNS rebinding";
    auto cross = a.req("GET", "/", {}, false);
    cross.headers["origin"] = "http://evil.example.com";
    EXPECT_EQ(a.handler.handle(cross).status, 403);
}

TEST(ApiUi, OnlyGetAndHeadAndTheApiStaysAuthenticated) {
    Api a;
    EXPECT_EQ(a.call("POST", "/", json{{"x", 1}}, false).status, 405);
    auto head = a.req("HEAD", "/", {}, false);
    auto hr = a.handler.handle(head);
    EXPECT_EQ(hr.status, 200);
    EXPECT_TRUE(hr.body.empty());
    EXPECT_EQ(a.call("GET", "/v1/models", false).status, 401);
    EXPECT_EQ(a.call("GET", "/hcs/v1/runtime", false).status, 401);
    // The page itself must not contain the token.
    EXPECT_EQ(a.call("GET", "/", false).body.find(kToken), std::string::npos);
}
#endif
