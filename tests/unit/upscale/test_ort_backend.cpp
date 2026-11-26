// ONNX Runtime backend: discovery, structured errors when the runtime/model is missing, and (opt-in) a real
// onnxruntime.dll run with SYNTHETIC nearest-neighbour ONNX models. No real super-resolution model is used.
//
//   HCS_TEST_ORT_DIR   directory holding a real onnxruntime.dll (+ its provider DLLs)
//   HCS_TEST_ONNX_DIR  directory produced by gen_nn_onnx.py (nn4x_dynamic.onnx, nn4x_fixed32.onnx)
// Without them the real-runtime tests are skipped (reported as skipped, never as passed).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

#include "hcs/upscale_ort.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::upscale;
namespace fs = std::filesystem;

namespace {
std::optional<fs::path> env_dir(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v)
        return std::nullopt;
    return fs::path(v);
}
Rgb8 pattern(std::uint32_t w, std::uint32_t h) {
    Rgb8 im;
    im.width = w;
    im.height = h;
    im.pixels.resize(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t i = 0; i < im.pixels.size(); ++i)
        im.pixels[i] = static_cast<std::uint8_t>((i * 37 + i / 7) % 256);
    return im;
}
}  // namespace

TEST(OrtDiscovery, FindsRuntimeAndModelOnlyInGivenDirs) {
    test::TempDir td;
    test::write_file(td / "a" / "onnxruntime.dll", "x");
    test::write_file(td / "b" / "z_model.onnx", "x");
    test::write_file(td / "b" / "a_model.ONNX", "x");
    test::write_file(td / "b" / "notes.txt", "x");
    EXPECT_TRUE(find_ort_runtime({td / "none", td / "b"}).empty());
    const auto rt = find_ort_runtime({td / "b", td / "a"});
    ASSERT_FALSE(rt.empty());
    EXPECT_EQ(rt.filename().string(), "onnxruntime.dll");
    EXPECT_EQ(find_upscale_model(td / "b").filename().string(), "a_model.ONNX");  // first by name
    EXPECT_TRUE(find_upscale_model(td / "a").empty());
    EXPECT_TRUE(find_upscale_model(td / "missing").empty());
}

TEST(OrtBackend, MissingRuntimeIsAStructuredUnsupportedError) {
    test::TempDir td;
    test::write_file(td / "m.onnx", "not a model");
    OrtConfig c;
    c.runtime_dirs = {td.path()};
    c.model_path = td / "m.onnx";
    OrtUpscaleEngine e(c);
    auto r = e.ready();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_NE(r.error().message.find("onnxruntime.dll"), std::string::npos);
    // Cached: the second call reports the same error without retrying.
    EXPECT_EQ(e.ready().error().message, r.error().message);
    std::vector<float> in(3 * 4 * 4), out(3 * 16 * 16);
    EXPECT_FALSE(e.run_tile(in.data(), 4, 4, out.data(), {}).ok());
    // And the full pipeline surfaces the same code instead of rendering first.
    auto up = upscale_image(e, pattern(8, 8), {}, {}, nullptr, nullptr);
    ASSERT_FALSE(up.ok());
    EXPECT_EQ(up.error().code, ErrorCode::Unsupported);
}

TEST(OrtBackend, MissingModelIsUnsupportedNotACrash) {
    test::TempDir td;
    OrtConfig c;
    c.runtime_dirs = {td.path()};
    test::write_file(td / "onnxruntime.dll", "MZ not a real dll");
    c.model_path = td / "absent.onnx";
    OrtUpscaleEngine e(c);
    auto r = e.ready();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
}

#ifdef _WIN32
TEST(OrtBackend, BogusDllIsRejectedWithoutCrashing) {
    test::TempDir td;
    test::write_file(td / "onnxruntime.dll", "MZ this is not a PE file");
    test::write_file(td / "m.onnx", "x");
    OrtConfig c;
    c.runtime_dirs = {td.path()};
    c.model_path = td / "m.onnx";
    OrtUpscaleEngine e(c);
    auto r = e.ready();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_NE(r.error().message.find("LoadLibrary"), std::string::npos);
}
#endif

TEST(OrtRealRuntime, DynamicSyntheticModelMatchesNearestNeighbour) {
    auto rt = env_dir("HCS_TEST_ORT_DIR");
    auto md = env_dir("HCS_TEST_ONNX_DIR");
    if (!rt || !md)
        GTEST_SKIP() << "HCS_TEST_ORT_DIR / HCS_TEST_ONNX_DIR not set: real ONNX Runtime run NOT RUN";
    OrtConfig c;
    c.runtime_dirs = {*rt};
    c.model_path = *md / "nn4x_dynamic.onnx";
    c.provider = "cpu";
    OrtUpscaleEngine e(c);
    auto r = e.ready();
    ASSERT_TRUE(r.ok()) << r.error().message;
    const auto mi = e.info();
    EXPECT_EQ(mi.scale, 4);
    EXPECT_EQ(mi.fixed_tile, 0u);
    EXPECT_EQ(mi.provider, "CPU");
    EXPECT_FALSE(e.runtime_version().empty());

    // One tile: output replicates every input pixel 4x4.
    std::vector<float> in(3 * 5 * 3);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i) / 50.0f;
    std::vector<float> out(3 * 20 * 12);
    ASSERT_TRUE(e.run_tile(in.data(), 5, 3, out.data(), {}).ok());
    for (std::size_t y = 0; y < 12; ++y)
        for (std::size_t x = 0; x < 20; ++x)
            for (int ch = 0; ch < 3; ++ch)
                ASSERT_FLOAT_EQ(out[(y * 20 + x) * 3 + ch], in[((y / 4) * 5 + (x / 4)) * 3 + ch]);

    // Whole pipeline, tiled, through the real runtime.
    Options o;
    o.tile = 48;
    o.overlap = 8;
    Stats st;
    auto up = upscale_image(e, pattern(100, 70), o, {}, nullptr, &st);
    ASSERT_TRUE(up.ok()) << up.error().message;
    EXPECT_GT(st.tiles, 1u);
    EXPECT_EQ(up.value().width, 3840u);
    EXPECT_EQ(st.provider, "CPU");
}

// Opt-in: a REAL 4x super-resolution model (Real-ESRGAN x4, float32 NCHW, dynamic H/W).
//   HCS_TEST_ORT_DIR      directory with onnxruntime.dll
//   HCS_TEST_REAL_ONNX    path of the real model file (e.g. D:\HCSModels\upscale\real_esrgan_x4.onnx)
namespace {
Rgb8 smooth_pattern(std::uint32_t w, std::uint32_t h) {
    Rgb8 im;
    im.width = w;
    im.height = h;
    im.pixels.resize(static_cast<std::size_t>(w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const bool edge = (x / 24 + y / 24) % 2 == 0;
            im.pixels[(static_cast<std::size_t>(y) * w + x) * 3 + 0] =
                static_cast<std::uint8_t>(40 + 170 * x / w);
            im.pixels[(static_cast<std::size_t>(y) * w + x) * 3 + 1] =
                static_cast<std::uint8_t>(40 + 170 * y / h);
            im.pixels[(static_cast<std::size_t>(y) * w + x) * 3 + 2] = edge ? 200 : 60;
        }
    return im;
}
}  // namespace

TEST(OrtRealModel, RealEsrganTileIsFiniteInRangeAndConsistentWithTheInput) {
    auto rt = env_dir("HCS_TEST_ORT_DIR");
    auto md = env_dir("HCS_TEST_REAL_ONNX");
    if (!rt || !md)
        GTEST_SKIP() << "HCS_TEST_ORT_DIR / HCS_TEST_REAL_ONNX not set: real super-resolution model NOT RUN";
    OrtConfig c;
    c.runtime_dirs = {*rt};
    c.model_path = *md;
    c.provider = "cpu";
    OrtUpscaleEngine e(c);
    auto r = e.ready();
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(e.info().scale, 4);
    EXPECT_EQ(e.info().fixed_tile, 0u);
    const Rgb8 src = smooth_pattern(48, 48);
    std::vector<float> in(3 * 48 * 48), out(3 * 192 * 192);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = src.pixels[i] / 255.0f;
    ASSERT_TRUE(e.run_tile(in.data(), 48, 48, out.data(), {}).ok());
    double err = 0.0;
    for (float v : out) {
        ASSERT_TRUE(std::isfinite(v));
        ASSERT_GT(v, -0.5f);
        ASSERT_LT(v, 1.5f);
    }
    // The 4x4 box average of the output must stay close to the input pixel (the model adds detail, it must
    // not change colours): mean absolute difference below 4/255.
    for (std::size_t y = 0; y < 192; ++y)
        for (std::size_t x = 0; x < 192; ++x)
            for (int ch = 0; ch < 3; ++ch) {
                if (y % 4 || x % 4)
                    continue;
                double acc = 0;
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 4; ++dx) acc += out[((y + dy) * 192 + (x + dx)) * 3 + ch];
                err += std::fabs(acc / 16.0 - in[((y / 4) * 48 + (x / 4)) * 3 + ch]);
            }
    EXPECT_LT(err / (48.0 * 48.0 * 3.0), 4.0 / 255.0);
}

TEST(OrtRealModel, RealEsrganTiledEqualsSingleTileWithoutSeams) {
    auto rt = env_dir("HCS_TEST_ORT_DIR");
    auto md = env_dir("HCS_TEST_REAL_ONNX");
    if (!rt || !md)
        GTEST_SKIP() << "HCS_TEST_ORT_DIR / HCS_TEST_REAL_ONNX not set: real super-resolution model NOT RUN";
    OrtConfig c;
    c.runtime_dirs = {*rt};
    c.model_path = *md;
    c.provider = "cpu";
    OrtUpscaleEngine e(c);
    ASSERT_TRUE(e.ready().ok());
    const Rgb8 src = smooth_pattern(96, 80);
    Options whole;
    whole.target_long_edge = 384;  // exactly 4x: the Lanczos stage is the identity
    whole.tile = 128;
    Options tiled = whole;
    tiled.tile = 48;
    tiled.overlap = 16;
    Stats sw, stl;
    auto a = upscale_image(e, src, whole, {}, nullptr, &sw);
    auto b = upscale_image(e, src, tiled, {}, nullptr, &stl);
    ASSERT_TRUE(a.ok()) << a.error().message;
    ASSERT_TRUE(b.ok()) << b.error().message;
    EXPECT_EQ(sw.tiles, 1u);
    EXPECT_GT(stl.tiles, 1u);
    ASSERT_EQ(a.value().pixels.size(), b.value().pixels.size());
    double sum = 0.0;
    int maxd = 0;
    for (std::size_t i = 0; i < a.value().pixels.size(); ++i) {
        const int d = std::abs(int(a.value().pixels[i]) - int(b.value().pixels[i]));
        sum += d;
        maxd = std::max(maxd, d);
    }
    const double mean = sum / static_cast<double>(a.value().pixels.size());
    // A convolutional model is not exactly translation invariant at tile borders, but with a 16 px overlap
    // and a linear feather the difference to a single pass must stay small (no visible seam).
    EXPECT_LT(mean, 1.5) << "mean abs diff " << mean << " max " << maxd;
    EXPECT_LT(maxd, 40);
}

TEST(OrtRealRuntime, FixedSizeSyntheticModelIsDetectedAndPadded) {
    auto rt = env_dir("HCS_TEST_ORT_DIR");
    auto md = env_dir("HCS_TEST_ONNX_DIR");
    if (!rt || !md)
        GTEST_SKIP() << "HCS_TEST_ORT_DIR / HCS_TEST_ONNX_DIR not set: real ONNX Runtime run NOT RUN";
    OrtConfig c;
    c.runtime_dirs = {*rt};
    c.model_path = *md / "nn4x_fixed32.onnx";
    c.provider = "cpu";
    OrtUpscaleEngine e(c);
    ASSERT_TRUE(e.ready().ok());
    EXPECT_EQ(e.info().fixed_tile, 32u);
    EXPECT_EQ(e.info().scale, 4);
    std::vector<float> in(3 * 16 * 16), out(3 * 64 * 64);
    EXPECT_EQ(e.run_tile(in.data(), 16, 16, out.data(), {}).error().code, ErrorCode::InvalidArgument);
    auto up = upscale_image(e, pattern(50, 40), {}, {}, nullptr, nullptr);
    ASSERT_TRUE(up.ok()) << up.error().message;
    EXPECT_EQ(up.value().width, 3840u);
}

TEST(OrtRealRuntime, ExplicitDirectMlWithoutTheProviderIsAnErrorNotASilentFallback) {
    auto rt = env_dir("HCS_TEST_ORT_DIR");
    auto md = env_dir("HCS_TEST_ONNX_DIR");
    if (!rt || !md)
        GTEST_SKIP() << "HCS_TEST_ORT_DIR / HCS_TEST_ONNX_DIR not set: real ONNX Runtime run NOT RUN";
    OrtConfig c;
    c.runtime_dirs = {*rt};
    c.model_path = *md / "nn4x_dynamic.onnx";
    c.provider = "dml";
    OrtUpscaleEngine e(c);
    auto r = e.ready();
    if (r.ok())
        GTEST_SKIP() << "this onnxruntime.dll has DirectML: " << e.info().provider;
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    // "auto" on the same runtime must fall back to CPU and SAY so.
    c.provider = "auto";
    OrtUpscaleEngine a(c);
    ASSERT_TRUE(a.ready().ok());
    EXPECT_NE(a.info().provider.find("CPU (DirectML unavailable"), std::string::npos) << a.info().provider;
}
