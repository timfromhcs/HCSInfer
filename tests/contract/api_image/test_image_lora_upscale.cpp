// Contract tests for the image LoRA library and the opt-in 4K upscale step on /v1/images/generations and
// GET /hcs/v1/loras. FakeImageEngine exercises the plumbing; the nearest-neighbour upscaler below is a unit
// test double (it proves the control-plane wiring, NOT upscaling quality; no ONNX model was run, see
// docs/test-evidence/wave-b/b3).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "hcs/api/data_uri.hpp"
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
    mutable std::atomic<int> tiles{0};
    std::uint64_t working_bytes = 4096;  // per input pixel (planner input)
    bool slow = false;                   // each tile blocks until the token is cancelled
    Result<void> ready() const override { return ready_result; }
    upscale::ModelInfo info() const override {
        upscale::ModelInfo m;
        m.scale = 4;
        m.name = "nn-test-double.onnx";
        m.provider = "test-double";
        m.working_bytes_per_input_pixel = working_bytes;
        return m;
    }
    Result<void> run_tile(const float* in, std::uint32_t w, std::uint32_t h, float* out,
                          const CancellationToken& token) override {
        ++tiles;
        if (slow) {
            while (!token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return make_error(ErrorCode::Cancelled, "cancelled in the tile");
        }
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
    EXPECT_EQ(a.up->tiles.load(), 0);
    auto off = a.post(Api::body({{"upscale", "off"}}));
    ASSERT_EQ(off.status, 200);
    EXPECT_EQ(a.up->tiles.load(), 0);
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
    EXPECT_GT(a.up->tiles.load(), 0);
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

// ---- upscale-only route (POST /hcs/v1/images/upscale) -------------------------------------------------------
namespace {

std::string b64_encode(const std::vector<std::uint8_t>& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (std::size_t i = 0; i < d.size(); i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(d[i]) << 16;
        if (i + 1 < d.size())
            v |= static_cast<std::uint32_t>(d[i + 1]) << 8;
        if (i + 2 < d.size())
            v |= d[i + 2];
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += i + 1 < d.size() ? t[(v >> 6) & 63] : '=';
        o += i + 2 < d.size() ? t[v & 63] : '=';
    }
    return o;
}

// Valid PNG of w x h (flat grey) made with the production encoder.
std::string png_b64(std::uint32_t w, std::uint32_t h) {
    upscale::Rgb8 img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<std::size_t>(w) * h * 3, 128);
    auto enc = upscale::encode_png(img);
    EXPECT_TRUE(enc.ok());
    return b64_encode(enc.value());
}

// A PNG signature + IHDR that claims w x h (no pixel data): enough for the header probe, never decodable.
std::string fake_ihdr_b64(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> d = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (std::uint32_t v : {w, h})
        for (int s = 24; s >= 0; s -= 8) d.push_back(static_cast<std::uint8_t>((v >> s) & 0xFF));
    for (int i = 0; i < 9; ++i) d.push_back(0);  // depth, colour type, ..., CRC (not checked by the probe)
    return b64_encode(d);
}

HttpResponse post_upscale(Api& a, const json& body) {
    return a.handler.handle(a.req("POST", "/hcs/v1/images/upscale", body.dump()));
}

}  // namespace

TEST(ApiUpscaleOnly, UpscalesAnExistingPngToA3840LongEdgeWithoutAModel) {
    Api a(true, true);
    auto r = post_upscale(a, {{"image", "data:image/png;base64," + png_b64(96, 64)}});
    ASSERT_EQ(r.status, 200) << r.body;
    const json j = json::parse(r.body);
    EXPECT_EQ(j["hcs"]["upscale"]["status"], "done");
    EXPECT_EQ(j["hcs"]["upscale"]["from_width"], 96);
    EXPECT_EQ(j["hcs"]["upscale"]["from_height"], 64);
    EXPECT_GT(j["hcs"]["upscale"]["tiles"].get<int>(), 0);
    EXPECT_GT(j["hcs"]["upscale"]["tile"].get<int>(), 0);
    EXPECT_EQ(j["hcs"]["width"], 3840);
    EXPECT_EQ(j["hcs"]["height"], 2560);
    EXPECT_FALSE(j["hcs"].contains("model")) << "no diffusion model is involved";
    const auto [w, h] = png_size_of(j["data"][0]["b64_json"].get<std::string>());
    EXPECT_EQ(w, 3840u);
    EXPECT_EQ(h, 2560u);
    EXPECT_EQ(a.fx.image.generations.load(), 0) << "the diffusion engine must not run";
}

TEST(ApiUpscaleOnly, PortraitKeepsTheAspectRatio) {
    Api a(true, true);
    auto r = post_upscale(a, {{"image", png_b64(40, 80)}});  // bare base64 is accepted too
    ASSERT_EQ(r.status, 200) << r.body;
    const json j = json::parse(r.body);
    EXPECT_EQ(j["hcs"]["width"], 1920);
    EXPECT_EQ(j["hcs"]["height"], 3840);
}

TEST(ApiUpscaleOnly, SourceAlreadyAtTheCapIsReportedSkippedNotUpscaled) {
    Api a(true, true);
    auto r = post_upscale(a, {{"image", png_b64(3840, 8)}});
    ASSERT_EQ(r.status, 200) << r.body;
    const json j = json::parse(r.body);
    EXPECT_EQ(j["hcs"]["upscale"]["status"], "skipped");
    EXPECT_EQ(j["hcs"]["width"], 3840);
    EXPECT_EQ(a.up->tiles.load(), 0);
}

TEST(ApiUpscaleOnly, NoUpscalerOrRuntimeIsUnsupportedWithTheReason) {
    Api none(true, /*with_upscaler=*/false);
    auto r = post_upscale(none, {{"image", png_b64(32, 32)}});
    EXPECT_EQ(r.status, 501) << r.body;
    EXPECT_EQ(err_of(r)["code"], "unsupported");

    Api missing(true, true);
    missing.up->ready_result = make_error(ErrorCode::Unsupported, "onnxruntime.dll not found");
    auto m = post_upscale(missing, {{"image", png_b64(32, 32)}});
    EXPECT_EQ(m.status, 501) << m.body;
    EXPECT_NE(err_of(m)["message"].get<std::string>().find("onnxruntime.dll"), std::string::npos);
    EXPECT_EQ(missing.up->tiles.load(), 0);
}

TEST(ApiUpscaleOnly, BadInputIsRejectedBeforeAnyDecodeOrAllocation) {
    Api a(true, true);
    EXPECT_EQ(post_upscale(a, json::object()).status, 400) << "image is required";
    EXPECT_EQ(post_upscale(a, {{"image", png_b64(32, 32)}, {"extra", 1}}).status, 400) << "unknown key";
    EXPECT_EQ(post_upscale(a, {{"image", png_b64(32, 32)}, {"upscale", "8k"}}).status, 400);
    EXPECT_EQ(post_upscale(a, {{"image", 5}}).status, 400);
    EXPECT_EQ(post_upscale(a, {{"image", "data:image/png;base64,!!!notbase64"}}).status, 400);
    // A JPEG header is not a PNG.
    const std::vector<std::uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1,
                                            0,    1,    0,    0,    0xFF, 0xC0, 0, 17, 8, 0, 32, 0, 32, 3, 1, 0x22, 0,
                                            2,    0x11, 1,    3,    0x11, 1};
    auto j = post_upscale(a, {{"image", b64_encode(jpeg)}});
    EXPECT_EQ(j.status, 400) << j.body;
    // 5000 x 5000 = 25 MP claimed in the header: over the 16 MP limit, refused without decoding.
    auto big = post_upscale(a, {{"image", fake_ihdr_b64(5000, 5000)}});
    EXPECT_EQ(big.status, 400) << big.body;
    EXPECT_NE(err_of(big)["message"].get<std::string>().find("pixel limit"), std::string::npos);
    EXPECT_EQ(a.up->tiles.load(), 0);
}

TEST(ApiUpscaleOnly, MethodMustBePost) {
    Api a(true, true);
    EXPECT_EQ(a.handler.handle(a.req("GET", "/hcs/v1/images/upscale")).status, 405);
}

TEST(ApiUpscaleOnly, BudgetThatCannotFitEvenTheSmallestTileIsRefusedUpFront) {
    Api a(true, true);
    a.up->working_bytes = 1ull << 40;  // an engine whose working set can never fit the budget
    auto r = post_upscale(a, {{"image", png_b64(64, 64)}});
    EXPECT_EQ(r.status, 503) << r.body;
    EXPECT_EQ(err_of(r)["code"], "out_of_memory");
    EXPECT_EQ(a.up->tiles.load(), 0) << "refused before any tile ran";
}

TEST(ApiUpscaleOnly, CancelStopsTheJobBetweenTilesAndReleasesTheSlot) {
    Api a(true, true);
    a.up->slow = true;  // each tile waits until the token is cancelled
    UpscaleParams p;
    auto raw = png_b64(64, 64);
    auto dec = decode_image_data_uri("data:image/png;base64," + raw, 1u << 20);
    ASSERT_TRUE(dec.ok());
    p.image = std::move(dec.value());
    auto sub = a.fx.cp->submit_upscale(p);
    ASSERT_TRUE(sub.ok()) << sub.error().message;
    const JobId id = sub.value().id;
    while (a.up->tiles.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(a.fx.cp->cancel_job(id).ok());
    ASSERT_TRUE(a.fx.cp->wait_job(id, std::chrono::seconds(10)));
    auto st = a.fx.cp->job_status(id);
    ASSERT_TRUE(st.ok());
    EXPECT_EQ(st.value().state, JobState::Cancelled);
    EXPECT_FALSE(a.fx.cp->image_result(id).ok()) << "no result is stored for a cancelled upscale";
    EXPECT_EQ(a.up->tiles.load(), 1) << "no further tile started after the cancel";
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
