// ONNX Runtime backend: discovery, structured errors when the runtime/model is missing, and (opt-in) a real
// onnxruntime.dll run with SYNTHETIC nearest-neighbour ONNX models. No real super-resolution model is used.
//
//   HCS_TEST_ORT_DIR   directory holding a real onnxruntime.dll (+ its provider DLLs)
//   HCS_TEST_ONNX_DIR  directory produced by gen_nn_onnx.py (nn4x_dynamic.onnx, nn4x_fixed32.onnx)
// Without them the real-runtime tests are skipped (reported as skipped, never as passed).
#include <gtest/gtest.h>

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
#ifdef _WIN32
    EXPECT_NE(r.error().message.find("onnxruntime.dll"), std::string::npos);
#else
    // The ONNX Runtime upscaler backend is Windows-only for now (documented Planned on Linux/macOS).
    EXPECT_NE(r.error().message.find("Windows only"), std::string::npos) << r.error().message;
#endif
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
