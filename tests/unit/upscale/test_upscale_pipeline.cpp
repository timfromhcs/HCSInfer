// Upscale pipeline math: tiling, feathering, memory planning, resampling. The nearest-neighbour "model" below is a
// UNIT TEST DOUBLE only; the production backend is the ONNX Runtime engine (see test_ort_backend.cpp).
#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <random>

#include "hcs/upscale.hpp"

using namespace hcs;
using namespace hcs::upscale;

namespace {

// Test double: nearest-neighbour s-times upscale; optionally adds a constant per-tile bias (to prove blending)
// and can pretend to be a fixed-tile model or poison its output.
class NnEngine final : public IUpscaleEngine {
public:
    int scale = 4;
    std::uint32_t fixed = 0;
    float bias_step = 0.0f;
    bool poison = false;
    int calls = 0;
    std::uint32_t max_w = 0, max_h = 0;
    int cancel_after = -1;
    CancellationSource* cancel_src = nullptr;
    Result<void> ready_result = {};

    Result<void> ready() const override { return ready_result; }
    ModelInfo info() const override {
        ModelInfo m;
        m.scale = scale;
        m.fixed_tile = fixed;
        m.name = "nn-test-double";
        m.provider = "test-double";
        return m;
    }
    Result<void> run_tile(const float* in, std::uint32_t w, std::uint32_t h, float* out,
                          const CancellationToken&) override {
        if (fixed && (w != fixed || h != fixed))
            return make_error(ErrorCode::InvalidArgument, "bad fixed tile");
        max_w = std::max(max_w, w);
        max_h = std::max(max_h, h);
        const float bias = bias_step * static_cast<float>(calls);
        ++calls;
        if (cancel_src && calls == cancel_after)
            cancel_src->request_cancel();
        const std::size_t ow = static_cast<std::size_t>(w) * scale;
        for (std::size_t y = 0; y < static_cast<std::size_t>(h) * scale; ++y)
            for (std::size_t x = 0; x < ow; ++x)
                for (int c = 0; c < 3; ++c)
                    out[(y * ow + x) * 3 + c] = in[((y / scale) * w + (x / scale)) * 3 + c] + bias;
        if (poison)
            out[0] = std::numeric_limits<float>::quiet_NaN();
        return {};
    }
};

Rgb8 gradient(std::uint32_t w, std::uint32_t h) {
    Rgb8 im;
    im.width = w;
    im.height = h;
    im.pixels.resize(static_cast<std::size_t>(w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = &im.pixels[(static_cast<std::size_t>(y) * w + x) * 3];
            p[0] = static_cast<std::uint8_t>((x * 7 + y * 3) % 256);
            p[1] = static_cast<std::uint8_t>((x * 2 + y * 11) % 256);
            p[2] = static_cast<std::uint8_t>((x * 5 + y * 5 + 40) % 256);
        }
    return im;
}

}  // namespace

// --- planning -----------------------------------------------------------------------------------------------

TEST(UpscaleTiles, SingleTileWhenImageFits) {
    auto t = plan_tiles(100, 80, 128, 16);
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].x, 0u);
    EXPECT_EQ(t[0].w, 100u);
    EXPECT_EQ(t[0].h, 80u);
}

TEST(UpscaleTiles, CoversEveryPixelWithOverlapAndAlignedLastTile) {
    const std::uint32_t W = 301, H = 130, T = 128, O = 16;
    auto tiles = plan_tiles(W, H, T, O);
    std::vector<int> cover(static_cast<std::size_t>(W) * H, 0);
    for (const auto& r : tiles) {
        EXPECT_LE(r.x + r.w, W);
        EXPECT_LE(r.y + r.h, H);
        EXPECT_EQ(r.w, T);
        for (std::uint32_t y = r.y; y < r.y + r.h; ++y)
            for (std::uint32_t x = r.x; x < r.x + r.w; ++x) ++cover[static_cast<std::size_t>(y) * W + x];
    }
    for (int c : cover) EXPECT_GE(c, 1);
    // Columns: 0, 112, 173 (= 301-128); rows: 0, 2 (= 130-128).
    EXPECT_EQ(tiles.size(), 3u * 2u);
    EXPECT_EQ(tiles[2].x, 173u);
    EXPECT_EQ(tiles[3].y, 2u);
    // Adjacent tiles overlap by at least `O` pixels.
    EXPECT_GE(tiles[0].x + tiles[0].w - tiles[1].x, O);
}

TEST(UpscaleTiles, DegenerateInputs) {
    EXPECT_TRUE(plan_tiles(0, 10, 64, 8).empty());
    EXPECT_TRUE(plan_tiles(10, 10, 0, 8).empty());
    auto t = plan_tiles(200, 10, 64, 999);  // overlap >= tile is clamped, never an endless loop
    EXPECT_FALSE(t.empty());
}

TEST(UpscaleTarget, LongEdgeExactAspectPreserved) {
    std::uint32_t w = 0, h = 0;
    target_size(512, 512, 3840, w, h);
    EXPECT_EQ(w, 3840u);
    EXPECT_EQ(h, 3840u);
    target_size(768, 512, 3840, w, h);
    EXPECT_EQ(w, 3840u);
    EXPECT_EQ(h, 2560u);
    target_size(512, 768, 3840, w, h);
    EXPECT_EQ(h, 3840u);
    EXPECT_EQ(w, 2560u);
    target_size(1000, 3, 3840, w, h);
    EXPECT_EQ(w, 3840u);
    EXPECT_EQ(h, 12u);
}

TEST(UpscalePlan, NeverUpscalesBeyondTheCap) {
    ModelInfo m;
    auto p = plan_upscale(3840, 2160, m, {});
    ASSERT_TRUE(p.ok());
    EXPECT_TRUE(p.value().skip);
    EXPECT_FALSE(p.value().skip_reason.empty());
    auto p2 = plan_upscale(4096, 100, m, {});
    ASSERT_TRUE(p2.ok());
    EXPECT_TRUE(p2.value().skip);
    auto p3 = plan_upscale(3839, 100, m, {});
    ASSERT_TRUE(p3.ok());
    EXPECT_FALSE(p3.value().skip);
}

TEST(UpscalePlan, TileSizeShrinksWithTheBudgetAndFailsClosed) {
    ModelInfo m;
    m.scale = 4;
    const std::uint32_t w = 512, h = 512;
    std::uint32_t ow = 0, oh = 0;
    target_size(w, h, 3840, ow, oh);
    // Tight budget: just above the cost of the 128 px tile, below the 192 px tile.
    const auto need128 = estimate_peak_bytes(w, h, 128, m, ow, oh);
    const auto need192 = estimate_peak_bytes(w, h, 192, m, ow, oh);
    ASSERT_LT(need128, need192);
    Options o;
    o.memory_budget_bytes = need128;
    auto p = plan_upscale(w, h, m, o);
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().tile, 128u);
    EXPECT_LE(p.value().peak_bytes, need128);
    // Generous budget: biggest candidate.
    o.memory_budget_bytes = 8ull << 30;
    EXPECT_EQ(plan_upscale(w, h, m, o).value().tile, 512u);
    // Below the smallest tile: OutOfMemory with numbers, never a silent oversize.
    o.memory_budget_bytes = estimate_peak_bytes(w, h, 64, m, ow, oh) - 1;
    auto bad = plan_upscale(w, h, m, o);
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, ErrorCode::OutOfMemory);
}

TEST(UpscalePlan, FixedTileModelsAndExplicitTile) {
    ModelInfo m;
    m.fixed_tile = 64;
    auto p = plan_upscale(200, 100, m, {});
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().tile, 64u);
    Options o;
    o.tile = 128;
    EXPECT_EQ(plan_upscale(200, 100, m, o).error().code, ErrorCode::InvalidArgument);
    ModelInfo dyn;
    o.tile = 16;
    EXPECT_EQ(plan_upscale(200, 100, dyn, o).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(plan_upscale(0, 100, dyn, {}).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(plan_upscale(5000, 5000, dyn, {}).error().code, ErrorCode::InvalidArgument);  // > 16 MP
}

TEST(UpscaleFeather, MonotonicPositiveAndSumsToOneAcrossAnOverlap) {
    EXPECT_FLOAT_EQ(feather_weight(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(feather_weight(100, 16), 1.0f);
    float prev = 0.0f;
    for (std::uint32_t i = 0; i < 16; ++i) {
        const float v = feather_weight(i, 16);
        EXPECT_GT(v, prev);
        EXPECT_GT(v, 0.0f);
        EXPECT_LT(v, 1.0f);
        prev = v;
    }
}

// --- pipeline ------------------------------------------------------------------------------------------------

TEST(UpscalePipeline, TiledResultEqualsSingleTileResultForAPositionIndependentModel) {
    // With a nearest-neighbour model every tile agrees on every pixel, so blending must reproduce the untiled
    // result exactly: any wrong tile offset, stride, clamp or weight normalisation would show up as a difference.
    const Rgb8 src = gradient(150, 90);
    Options tiled;
    tiled.tile = 64;
    tiled.overlap = 16;
    Options whole;
    whole.tile = 2048;
    NnEngine e1, e2;
    Stats s1, s2;
    auto a = upscale_image(e1, src, tiled, {}, nullptr, &s1);
    auto b = upscale_image(e2, src, whole, {}, nullptr, &s2);
    ASSERT_TRUE(a.ok()) << a.error().message;
    ASSERT_TRUE(b.ok()) << b.error().message;
    EXPECT_GT(s1.tiles, 1u);
    EXPECT_EQ(s2.tiles, 1u);
    EXPECT_EQ(a.value().width, 3840u);
    EXPECT_EQ(a.value().height, 2304u);  // 3840 * 90 / 150
    // Blending identical values is exact up to float rounding (sum(w*v)/sum(w)); after 8-bit rounding that can
    // flip a sample sitting on a .5 boundary by one level, never more.
    ASSERT_EQ(a.value().pixels.size(), b.value().pixels.size());
    std::size_t off_by_one = 0;
    for (std::size_t i = 0; i < a.value().pixels.size(); ++i) {
        const int d = std::abs(int(a.value().pixels[i]) - int(b.value().pixels[i]));
        ASSERT_LE(d, 1) << "sample " << i;
        off_by_one += d;
    }
    EXPECT_LT(off_by_one, a.value().pixels.size() / 5000) << "boundary flips should be rare";
    EXPECT_EQ(s1.provider, "test-double");
}

TEST(UpscalePipeline, ConstantImageStaysConstantAfterBlendAndResample) {
    Rgb8 src;
    src.width = 70;
    src.height = 50;
    src.pixels.assign(70 * 50 * 3, 77);
    NnEngine e;
    Options o;
    o.tile = 32;
    o.overlap = 8;
    auto r = upscale_image(e, src, o, {}, nullptr, nullptr);
    ASSERT_TRUE(r.ok());
    for (auto v : r.value().pixels) ASSERT_EQ(v, 77);
}

TEST(UpscalePipeline, SeamBlendIsSmoothWhenTilesDisagree) {
    // Each tile call adds a different constant bias (0.00, 0.08, ...). With a hard seam the jump between adjacent
    // tiles would be the full 0.08; the feather must spread it over the overlap so no neighbouring output pixels
    // differ by more than ~one ramp step.
    Rgb8 src;
    src.width = 96;
    src.height = 16;
    src.pixels.assign(96 * 16 * 3, 100);
    NnEngine e;
    e.scale = 2;
    e.bias_step = 0.08f;
    Options o;
    o.target_long_edge = 192;  // 2x model pass, target == model size => the Lanczos step is an identity resample
    o.tile = 64;
    o.overlap = 16;
    auto r = upscale_image(e, src, o, {}, nullptr, nullptr);
    ASSERT_TRUE(r.ok()) << r.error().message;
    ASSERT_EQ(r.value().width, 192u);
    int max_step = 0;
    const std::uint8_t* row = &r.value().pixels[(8 * 192) * 3];
    for (std::uint32_t x = 1; x < 192; ++x) max_step = std::max(max_step, std::abs(int(row[x * 3]) - int(row[(x - 1) * 3])));
    // Bias difference 0.08 * 255 = 20 levels; blended over a 32 px ramp the per-pixel step is ~1.
    EXPECT_LE(max_step, 2);
    // And the seam region really moved between the two tile values (not a hard switch somewhere else).
    EXPECT_NE(row[0], row[191 * 3]);
}

TEST(UpscalePipeline, FixedTileModelWithPaddingGivesTheSameImage) {
    const Rgb8 src = gradient(45, 30);  // smaller than the fixed tile in both dimensions
    NnEngine fixed, dyn;
    fixed.fixed = 64;
    Options o;
    auto a = upscale_image(fixed, src, o, {}, nullptr, nullptr);
    auto b = upscale_image(dyn, src, o, {}, nullptr, nullptr);
    ASSERT_TRUE(a.ok()) << a.error().message;
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().pixels, b.value().pixels);
    EXPECT_EQ(fixed.max_w, 64u);
}

TEST(UpscalePipeline, SkipsAtTheCapWithoutCallingTheModel) {
    Rgb8 big;
    big.width = 3840;
    big.height = 8;
    big.pixels.assign(3840 * 8 * 3, 1);
    NnEngine e;
    Stats st;
    auto r = upscale_image(e, big, {}, {}, nullptr, &st);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(st.skipped);
    EXPECT_EQ(e.calls, 0);
    EXPECT_EQ(r.value().pixels, big.pixels);
}

TEST(UpscalePipeline, CancellationBetweenTilesAndStructuredErrors) {
    const Rgb8 src = gradient(150, 90);
    CancellationSource cs;
    NnEngine e;
    e.cancel_src = &cs;
    e.cancel_after = 2;
    Options o;
    o.tile = 64;
    auto r = upscale_image(e, src, o, cs.token(), nullptr, nullptr);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(e.calls, 2);  // stopped right after the tile that triggered the cancel

    NnEngine missing;
    missing.ready_result = make_error(ErrorCode::Unsupported, "onnxruntime.dll was not found");
    auto m = upscale_image(missing, src, {}, {}, nullptr, nullptr);
    ASSERT_FALSE(m.ok());
    EXPECT_EQ(m.error().code, ErrorCode::Unsupported);

    NnEngine nan;
    nan.poison = true;
    auto n = upscale_image(nan, src, o, {}, nullptr, nullptr);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, ErrorCode::BackendError);

    Rgb8 bad = src;
    bad.pixels.pop_back();
    EXPECT_EQ(upscale_image(e, bad, {}, {}, nullptr, nullptr).error().code, ErrorCode::InvalidArgument);
}

TEST(UpscalePipeline, ProgressIsMonotonicAndEndsAtOne) {
    const Rgb8 src = gradient(150, 90);
    NnEngine e;
    Options o;
    o.tile = 64;
    std::vector<double> seen;
    auto r = upscale_image(e, src, o, {}, [&](double p) { seen.push_back(p); }, nullptr);
    ASSERT_TRUE(r.ok());
    ASSERT_GE(seen.size(), 2u);
    for (std::size_t i = 1; i < seen.size(); ++i) EXPECT_GE(seen[i], seen[i - 1]);
    EXPECT_DOUBLE_EQ(seen.back(), 1.0);
}

// --- resampling ---------------------------------------------------------------------------------------------

namespace {
std::vector<float> to_float(const Rgb8& im) {
    std::vector<float> f(im.pixels.size());
    for (std::size_t i = 0; i < f.size(); ++i) f[i] = im.pixels[i] / 255.0f;
    return f;
}
}  // namespace

TEST(UpscaleResample, IdentityAndConstant) {
    const Rgb8 src = gradient(33, 21);
    auto f = to_float(src);
    auto same = resample_lanczos(f.data(), 33, 21, 33, 21);
    EXPECT_EQ(same.pixels, src.pixels);  // taps collapse to a delta: exact
    std::vector<float> flat(40 * 30 * 3, 0.4f);
    auto up = resample_lanczos(flat.data(), 40, 30, 97, 61);
    for (auto v : up.pixels) ASSERT_EQ(v, 102);  // round(0.4 * 255)
    auto down = resample_lanczos(flat.data(), 40, 30, 7, 5);
    for (auto v : down.pixels) ASSERT_EQ(v, 102);
}

TEST(UpscaleResample, LinearRampSurvivesDownscale) {
    // A horizontal linear ramp is reproduced by Lanczos (partition of unity + symmetry) away from the borders.
    const std::uint32_t W = 200, H = 4;
    std::vector<float> f(W * H * 3);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) f[(y * W + x) * 3 + c] = static_cast<float>(x) / (W - 1);
    auto out = resample_lanczos(f.data(), W, H, 50, 4);
    for (std::uint32_t x = 4; x < 46; ++x) {
        const double centre = (x + 0.5) * 4.0 - 0.5;  // source coordinate of output pixel x
        const double expect = centre / (W - 1) * 255.0;
        EXPECT_NEAR(out.pixels[x * 3], expect, 1.5) << "x=" << x;
    }
}

// Golden values from a numpy implementation of the same Lanczos-3 definition (tests/unit/upscale/gen_resample_golden.py).
TEST(UpscaleResample, MatchesNumpyGolden) {
#include "resample_golden.inc"
    std::vector<float> in(kGoldenIn.begin(), kGoldenIn.end());
    auto up = resample_lanczos(in.data(), kGoldenInW, kGoldenInH, kGoldenUpW, kGoldenUpH);
    ASSERT_EQ(up.pixels.size(), kGoldenUp.size());
    for (std::size_t i = 0; i < kGoldenUp.size(); ++i) EXPECT_NEAR(up.pixels[i], kGoldenUp[i], 1) << "up i=" << i;
    auto down = resample_lanczos(in.data(), kGoldenInW, kGoldenInH, kGoldenDownW, kGoldenDownH);
    ASSERT_EQ(down.pixels.size(), kGoldenDown.size());
    for (std::size_t i = 0; i < kGoldenDown.size(); ++i) EXPECT_NEAR(down.pixels[i], kGoldenDown[i], 1) << "down i=" << i;
}

#ifdef HCS_UPSCALE_HAVE_STB
TEST(UpscalePng, RoundTripAndRejectGarbage) {
    const Rgb8 src = gradient(37, 23);
    auto enc = encode_png(src);
    ASSERT_TRUE(enc.ok());
    auto dec = decode_png(enc.value().data(), enc.value().size());
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.value().pixels, src.pixels);
    const std::uint8_t junk[16] = {1, 2, 3};
    EXPECT_FALSE(decode_png(junk, sizeof(junk)).ok());
    EXPECT_FALSE(decode_png(nullptr, 0).ok());
}

TEST(UpscalePng, UpscalePngEndToEndAndSkipKeepsBytes) {
    NnEngine e;
    auto png = encode_png(gradient(40, 30)).value();
    Stats st;
    auto out = upscale_png(e, png, {}, {}, nullptr, &st);
    ASSERT_TRUE(out.ok()) << out.error().message;
    auto dec = decode_png(out.value().data(), out.value().size());
    ASSERT_TRUE(dec.ok());
    EXPECT_EQ(dec.value().width, 3840u);
    EXPECT_EQ(dec.value().height, 2880u);
    Rgb8 big;
    big.width = 3840;
    big.height = 4;
    big.pixels.assign(3840 * 4 * 3, 9);
    auto bigpng = encode_png(big).value();
    auto kept = upscale_png(e, bigpng, {}, {}, nullptr, &st);
    ASSERT_TRUE(kept.ok());
    EXPECT_TRUE(st.skipped);
    EXPECT_EQ(kept.value(), bigpng);
}
#endif
