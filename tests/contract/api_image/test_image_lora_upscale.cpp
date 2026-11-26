// Contract tests for the image LoRA library and the opt-in 4K upscale step on /v1/images/generations and
// GET /hcs/v1/loras. FakeImageEngine exercises the plumbing; the nearest-neighbour upscaler below is a unit
// test double (it proves the control-plane wiring, NOT upscaling quality; no ONNX model was run, see
// docs/test-evidence/wave-b/b3).
#include <gtest/gtest.h>

#include <cstring>

#include "hcs/api/http_api.hpp"
#include "hcs/upscale.hpp"
#include "image_api_fixture.hpp"
#include "safetensors_builder.hpp"

using namespace hcs;
using namespace hcs::api;
using nlohmann::json;

namespace {

const char* kToken = "test-token-0123456789abcdef";

// Nearest-neighbour x4 "model": test double only.
class NnUpscaler final : public upscale::IUpscaleEngine {
public:
    Result<void> ready_result;
    bool fail_tiles = false;
    mutable int tiles = 0;
    Result<void> ready() const override { return ready_result; }
    upscale::ModelInfo info() const override {
        upscale::ModelInfo m;
        m.scale = 4;
        m.name = "nn-test-double.onnx";
        m.provider = "test-double";
        return m;
    }
    Result<void> run_tile(const float* in, std::uint32_t w, std::uint32_t h, float* out,
                          const CancellationToken&) override {
        ++tiles;
        if (fail_tiles)
            return make_error(ErrorCode::BackendError, "synthetic tile failure");
        const std::uint32_t ow = w * 4;
        for (std::uint32_t y = 0; y < h * 4; ++y)
            for (std::uint32_t x = 0; x < ow; ++x)
                std::memcpy(out + (static_cast<std::size_t>(y) * ow + x) * 3,
                            in + (static_cast<std::size_t>(y / 4) * w + x / 4) * 3, 3 * sizeof(float));
        return {};
    }
};

std::string lora_file(const std::vector<std::string>& names) {
    std::vector<test::StBuildTensor> ts;
    for (const auto& n : names) ts.push_back(test::f32_tensor(n, {2, 2}, {1, 2, 3, 4}));
    return test::build_safetensors(ts);
}

struct Api {
    test::ImageApiFixture fx;
    std::shared_ptr<NnUpscaler> up = std::make_shared<NnUpscaler>();
    ApiHandler handler;

    static ApiOptions options() {
        ApiOptions o;
        o.token = kToken;
        o.port = 8765;
        return o;
    }
    explicit Api(bool with_library = true, bool with_upscaler = false)
        : fx(true, {}), handler(*fx.cp, options(), fx.logger) {
        if (with_library) {
            const auto dir = fx.root / "lora" / "image";
            test::write_file(dir / "style-sd15.safetensors",
                             lora_file({"lora_unet_down_blocks_0.lora_down.weight",
                                        "lora_unet_down_blocks_0.lora_up.weight"}));
            test::write_file(dir / "klein-flux.safetensors",
                             lora_file({"lora_unet_double_blocks_0_img_attn_qkv.lora_down.weight"}));
            test::write_file(dir / "manifest.json",
                             R"({"schema_version":1,"loras":[
                    {"id":"style-sd15","file":"style-sd15.safetensors","name":"Style","base_family":"sd15",
                     "trigger_words":["sty"],"recommended_weight":0.8,"licence":"CC0-1.0","source":"synthetic"},
                    {"id":"klein-flux","file":"klein-flux.safetensors","name":"Klein","base_family":"flux3",
                     "trigger_words":[],"recommended_weight":1.0,"licence":"CC0-1.0","source":"synthetic"}]})");
            EXPECT_TRUE(fx.cp->scan_loras(dir).ok());
        }
        // Registered models whose ids reveal the family (the engine repeats the check on the loaded weights).
        for (const char* id : {"sd15-model", "flux3-model"}) {
            ModelManifest m;
            m.id = id;
            m.display_name = id;
            m.path = (fx.root / "sd-weights.gguf").string();
            m.format = ModelFormat::Gguf;
            m.kind = ModelKind::Image;
            m.size_bytes = 64;
            (void)fx.registry.add(m);
        }
        if (with_upscaler)
            fx.cp->set_upscaler(up);
    }
    HttpRequest req(const std::string& method, const std::string& path, const std::string& body = {}) {
        HttpRequest r;
        r.method = method;
        r.path = path;
        r.body = body;
        r.headers["host"] = "127.0.0.1:8765";
        r.headers["authorization"] = std::string("Bearer ") + kToken;
        if (!body.empty())
            r.headers["content-type"] = "application/json";
        return r;
    }
    HttpResponse post(const json& body) {
        return handler.handle(req("POST", "/v1/images/generations", body.dump()));
    }
    static json body(const json& extra, const std::string& model = "sd15-model") {
        // 512 px / 8 steps stay inside the sd15 and flux3 quality envelopes (the fake engine renders a 64 px
        // PNG).
        json b = {
            {"model", model}, {"prompt", "a red apple"}, {"size", "512x512"}, {"steps", 8}, {"seed", 5}};
        for (auto it = extra.begin(); it != extra.end(); ++it) b[it.key()] = it.value();
        return b;
    }
};

json err_of(const HttpResponse& r) {
    return json::parse(r.body)["error"];
}

// Reads width/height from the PNG IHDR of a base64 payload (first 24 bytes are enough).
std::pair<std::uint32_t, std::uint32_t> png_size_of(const std::string& b64) {
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> raw;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : b64.substr(0, 64)) {
        const auto p = tbl.find(c);
        if (p == std::string::npos)
            break;
        acc = (acc << 6) | static_cast<std::uint32_t>(p);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    auto be = [&](int o) {
        return (static_cast<std::uint32_t>(raw[o]) << 24) | (static_cast<std::uint32_t>(raw[o + 1]) << 16) |
               (static_cast<std::uint32_t>(raw[o + 2]) << 8) | raw[o + 3];
    };
    return {be(16), be(20)};
}

}  // namespace

// ---- LoRA ------------------------------------------------------------------------------------------------
TEST(ApiLoras, ListsLibraryWithoutAbsolutePaths) {
    Api a;
    auto r = a.handler.handle(a.req("GET", "/hcs/v1/loras"));
    ASSERT_EQ(r.status, 200) << r.body;
    const json j = json::parse(r.body);
    ASSERT_EQ(j["loras"].size(), 2u);
    EXPECT_EQ(j["max_per_request"], 4);
    bool saw = false;
    for (const auto& l : j["loras"])
        if (l["id"] == "style-sd15") {
            saw = true;
            EXPECT_EQ(l["base_family"], "sd15");
            EXPECT_EQ(l["status"], "ready");
            EXPECT_TRUE(l["usable"].get<bool>());
            EXPECT_NEAR(l["recommended_weight"].get<double>(), 0.8, 1e-6);
        }
    EXPECT_TRUE(saw);
    EXPECT_EQ(r.body.find(a.fx.root.string()), std::string::npos) << "absolute paths must never be exposed";
    EXPECT_EQ(r.body.find("\\\\"), std::string::npos);
}

TEST(ApiLoras, NoLibraryListsEmptyWithANote) {
    Api a(/*with_library=*/false);
    auto r = a.handler.handle(a.req("GET", "/hcs/v1/loras"));
    ASSERT_EQ(r.status, 200);
    const json j = json::parse(r.body);
    EXPECT_TRUE(j["loras"].empty());
    EXPECT_FALSE(j["note"].get<std::string>().empty());
}

TEST(ApiLoras, ResolvedSpecReachesTheEngineWithRecommendedWeightAndApprovedPath) {
    Api a;
    auto r = a.post(Api::body({{"loras", json::array({json{{"id", "style-sd15"}}})}}));
    ASSERT_EQ(r.status, 200) << r.body;
    const json j = json::parse(r.body);
    ASSERT_EQ(j["hcs"]["loras"].size(), 1u);
    EXPECT_EQ(j["hcs"]["loras"][0], "style-sd15:0.8");
    ASSERT_EQ(a.fx.image.last.loras.size(), 1u);
    const auto& s = a.fx.image.last.loras[0];
    EXPECT_EQ(s.id, "style-sd15");
    EXPECT_EQ(s.base_family, "sd15");
    EXPECT_NEAR(s.weight, 0.8f, 1e-6f);
    EXPECT_EQ(s.path.rfind(a.fx.root.string(), 0), 0u) << "path must be inside the approved root: " << s.path;
}

TEST(ApiLoras, ExplicitWeightIsHonouredAndBounded) {
    Api a;
    auto ok = a.post(Api::body({{"loras", json::array({json{{"id", "style-sd15"}, {"weight", 1.5}}})}}));
    ASSERT_EQ(ok.status, 200) << ok.body;
    EXPECT_NEAR(a.fx.image.last.loras[0].weight, 1.5f, 1e-6f);
    a.fx.image.generations = 0;
    for (double w : {2.5, -2.5}) {
        auto r = a.post(Api::body({{"loras", json::array({json{{"id", "style-sd15"}, {"weight", w}}})}}));
        EXPECT_EQ(r.status, 400) << w << ": " << r.body;
    }
    // 0 passes the schema but would be a silent no-op: rejected by the control plane.
    auto zero = a.post(Api::body({{"loras", json::array({json{{"id", "style-sd15"}, {"weight", 0}}})}}));
    EXPECT_EQ(zero.status, 400) << zero.body;
    EXPECT_EQ(a.fx.image.generations.load(), 0) << "rejected before the engine is touched";
}

TEST(ApiLoras, IncompatibleBaseFamilyIsAStructuredErrorBeforeRendering) {
    Api a;
    auto r = a.post(Api::body({{"loras", json::array({json{{"id", "style-sd15"}}})}}, "flux3-model"));
    ASSERT_EQ(r.status, 400) << r.body;
    EXPECT_NE(err_of(r)["message"].get<std::string>().find("lora_incompatible"), std::string::npos) << r.body;
    EXPECT_EQ(a.fx.image.generations.load(), 0);
    auto r2 = a.post(Api::body({{"loras", json::array({json{{"id", "klein-flux"}}})}}, "sd15-model"));
    EXPECT_EQ(r2.status, 400) << r2.body;
    EXPECT_EQ(a.fx.image.generations.load(), 0);
}

TEST(ApiLoras, UnknownIdDuplicateAndCountAreRejected) {
    Api a;
    EXPECT_EQ(a.post(Api::body({{"loras", json::array({json{{"id", "nope"}}})}})).status, 404);
    EXPECT_EQ(
        a.post(Api::body({{"loras", json::array({json{{"id", "style-sd15"}}, json{{"id", "style-sd15"}}})}}))
            .status,
        400);
    json five = json::array();
    for (int i = 0; i < 5; ++i) five.push_back(json{{"id", "style-sd15"}});
    EXPECT_EQ(a.post(Api::body({{"loras", five}})).status, 400);
    // paths are not an accepted field: ids only
    EXPECT_EQ(a.post(Api::body({{"loras",
                                 json::array({json{{"id", "style-sd15"}, {"path", "C:\\x.safetensors"}}})}}))
                  .status,
              400);
    EXPECT_EQ(a.fx.image.generations.load(), 0);
}

TEST(ApiLoras, RequestWithoutLibraryIsUnsupportedNotSilentlyIgnored) {
    Api a(/*with_library=*/false);
    auto r = a.post(Api::body({{"loras", json::array({json{{"id", "style-sd15"}}})}}));
    EXPECT_EQ(r.status, 501) << r.body;
    EXPECT_EQ(err_of(r)["code"], "unsupported");
    EXPECT_EQ(a.fx.image.generations.load(), 0);
}

TEST(ApiLoras, RequestWithoutLorasLeavesTheEngineRequestEmpty) {
    Api a;
    ASSERT_EQ(a.post(Api::body({})).status, 200);
    EXPECT_TRUE(a.fx.image.last.loras.empty());
    EXPECT_FALSE(json::parse(a.post(Api::body({})).body)["hcs"].contains("loras"));
}

// ---- upscale ---------------------------------------------------------------------------------------------
TEST(ApiUpscale, OffIsTheDefaultAndNeverTouchesTheUpscaler) {
    Api a(true, /*with_upscaler=*/true);
    auto r = a.post(Api::body({}));
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_FALSE(json::parse(r.body)["hcs"].contains("upscale"));
    EXPECT_EQ(a.up->tiles, 0);
    auto off = a.post(Api::body({{"upscale", "off"}}));
    ASSERT_EQ(off.status, 200);
    EXPECT_EQ(a.up->tiles, 0);
}

TEST(ApiUpscale, FourKProducesA3840LongEdgeImage) {
    Api a(true, true);
    auto r = a.post(Api::body({{"upscale", "4k"}}));
    ASSERT_EQ(r.status, 200) << r.body;
    const json j = json::parse(r.body);
    EXPECT_EQ(j["hcs"]["upscale"]["status"], "done");
    EXPECT_EQ(j["hcs"]["upscale"]["provider"], "test-double");
    EXPECT_EQ(j["hcs"]["upscale"]["from_width"], 64);  // the fake engine renders a 64 px PNG
    EXPECT_EQ(j["hcs"]["width"], 3840);
    EXPECT_EQ(j["hcs"]["height"], 3840);
    const auto [w, h] = png_size_of(j["data"][0]["b64_json"].get<std::string>());
    EXPECT_EQ(w, 3840u);
    EXPECT_EQ(h, 3840u);
    EXPECT_GT(a.up->tiles, 0);
}

TEST(ApiUpscale, MissingUpscalerOrRuntimeIsUnsupportedBeforeTheRender) {
    Api none(true, /*with_upscaler=*/false);
    auto r = none.post(Api::body({{"upscale", "4k"}}));
    EXPECT_EQ(r.status, 501) << r.body;
    EXPECT_EQ(err_of(r)["code"], "unsupported");
    EXPECT_EQ(none.fx.image.generations.load(), 0) << "must fail before a long render starts";

    Api missing(true, true);
    missing.up->ready_result =
        make_error(ErrorCode::Unsupported, "onnxruntime.dll not found in any approved dir");
    auto m = missing.post(Api::body({{"upscale", "4k"}}));
    EXPECT_EQ(m.status, 501) << m.body;
    EXPECT_NE(err_of(m)["message"].get<std::string>().find("onnxruntime.dll"), std::string::npos);
    EXPECT_EQ(missing.fx.image.generations.load(), 0);
}

TEST(ApiUpscale, InvalidValueIsRejected) {
    Api a(true, true);
    EXPECT_EQ(a.post(Api::body({{"upscale", "8k"}})).status, 400);
    EXPECT_EQ(a.post(Api::body({{"upscale", true}})).status, 400);
    EXPECT_EQ(a.fx.image.generations.load(), 0);
}

TEST(ApiUpscale, TileFailureKeepsTheRenderAndSaysWhy) {
    Api a(true, true);
    a.up->fail_tiles = true;
    auto r = a.post(Api::body({{"upscale", "4k"}}));
    ASSERT_EQ(r.status, 200) << r.body;
    const json j = json::parse(r.body);
    EXPECT_EQ(j["hcs"]["upscale"]["status"], "failed");
    EXPECT_NE(j["hcs"]["upscale"]["error"].get<std::string>().find("synthetic tile failure"),
              std::string::npos);
    EXPECT_EQ(j["hcs"]["width"], 512) << "the un-upscaled image is returned";
    const auto [w, h] = png_size_of(j["data"][0]["b64_json"].get<std::string>());
    EXPECT_EQ(w, 64u);
    (void)h;
}
