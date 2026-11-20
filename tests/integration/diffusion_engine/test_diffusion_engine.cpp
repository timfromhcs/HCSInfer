// Integration tests for the stable-diffusion.cpp adapter through the BLOCKING entry points used by the
// HTTP/MCP image routes (IImageStreamEngine). Real model required: set HCS_TEST_DIFFUSION_MODEL=<small
// diffusion GGUF>. Output is verified structurally (PNG signature, size); image quality is not judged.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "hcs/engines/diffusion_engine.hpp"
#include "hcs/job_queue.hpp"

using namespace hcs;
using namespace hcs::engines;

namespace {

std::string env(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

#define REQUIRE_MODEL_OR_SKIP(path_var)                           \
    const std::string path_var = env("HCS_TEST_DIFFUSION_MODEL"); \
    if (path_var.empty())                                         \
    GTEST_SKIP() << "NOT RUN: HCS_TEST_DIFFUSION_MODEL is not set (real-model test)"

bool is_png(const api::ImageResult& r) {
    static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return r.bytes.size() > 8 && std::equal(sig, sig + 8, r.bytes.begin());
}

struct Loaded {
    JobQueue queue;
    DiffusionImageEngine engine{queue};
    explicit Loaded(const std::string& path) {
        LoadRequest lr;
        lr.model_path = path;
        lr.model_id = "test";
        lr.backend = Backend::Cpu;
        auto r = engine.load(lr);
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
    }
    ~Loaded() {
        (void)engine.unload();
        queue.shutdown();
    }
};

ImageRequest small_request() {
    ImageRequest q;
    q.prompt = "a simple red apple on a plain table";
    q.width = 256;
    q.height = 256;
    q.steps = 2;
    q.seed = 7;
    return q;
}

}  // namespace

// Regression: the blocking entry points pass an EMPTY progress sink; the progress callback used to call it
// anyway and every blocking generation failed with "bad function call".
TEST(DiffusionBlocking, GenerateWithoutAProgressSinkProducesAPng) {
    REQUIRE_MODEL_OR_SKIP(path);
    Loaded m(path);
    auto r = m.engine.generate_image(small_request(), CancellationToken{});
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(is_png(r.value()));
    EXPECT_EQ(r.value().width, 256u);
    EXPECT_EQ(r.value().height, 256u);
    EXPECT_EQ(r.value().steps, 2u);
}

TEST(DiffusionBlocking, EditWithoutAProgressSinkKeepsTheInputSize) {
    REQUIRE_MODEL_OR_SKIP(path);
    Loaded m(path);
    auto base = m.engine.generate_image(small_request(), CancellationToken{});
    ASSERT_TRUE(base) << base.error().message;
    ImageRequest q = small_request();
    q.prompt = "a simple green apple on a plain table";
    q.strength = 0.6f;
    q.init_image = ImageInput{base.value().bytes};
    auto r = m.engine.edit_image(q, CancellationToken{});
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(is_png(r.value()));
    EXPECT_EQ(r.value().width, 256u);
    EXPECT_NE(r.value().bytes, base.value().bytes);
}

TEST(DiffusionBlocking, ProgressSinkStillReceivesProgress) {
    REQUIRE_MODEL_OR_SKIP(path);
    Loaded m(path);
    std::atomic<int> calls{0};
    auto r = m.engine.generate_image(small_request(), CancellationToken{}, [&](double f, std::string) {
        EXPECT_GE(f, 0.0);
        EXPECT_LE(f, 1.0);
        ++calls;
    });
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_GT(calls.load(), 0);
}

TEST(DiffusionBlocking, CancellationStopsTheRender) {
    REQUIRE_MODEL_OR_SKIP(path);
    Loaded m(path);
    CancellationSource src;
    ImageRequest q = small_request();
    q.width = q.height = 512;
    q.steps = 60;  // minutes on CPU unless cancelled
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        src.request_cancel();
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto r = m.engine.generate_image(q, src.token());
    t.join();
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_FALSE(r) << "a cancelled render must not return an image";
    if (!r)
        EXPECT_EQ(r.error().code, ErrorCode::Cancelled) << r.error().message;
    EXPECT_LT(secs, 120.0) << "cancellation must stop the render promptly";
}

TEST(DiffusionBlocking, InvalidRequestsAreRejectedBeforeRendering) {
    REQUIRE_MODEL_OR_SKIP(path);
    Loaded m(path);
    ImageRequest q = small_request();
    q.width = 250;
    EXPECT_FALSE(m.engine.generate_image(q, CancellationToken{}));
    q = small_request();
    q.steps = 0;
    EXPECT_FALSE(m.engine.generate_image(q, CancellationToken{}));
    q = small_request();
    q.prompt.clear();
    EXPECT_FALSE(m.engine.generate_image(q, CancellationToken{}));
    EXPECT_FALSE(m.engine.edit_image(small_request(), CancellationToken{})) << "edit needs an init image";
}

TEST(DiffusionBlocking, NotLoadedIsAnErrorNotACrash) {
    JobQueue queue;
    DiffusionImageEngine engine(queue);
    auto r = engine.generate_image(small_request(), CancellationToken{});
    EXPECT_FALSE(r);
    queue.shutdown();
}

// ---------------------------------------------------------------------------------------------
// FLUX.3 Klein multi-file loading and presets (AGENTS.md 17.2). The structural tests below need no model.
// ---------------------------------------------------------------------------------------------
namespace {

struct TempDir {
    std::filesystem::path dir;
    TempDir() {
        dir = std::filesystem::temp_directory_path() / ("hcs-diff-test-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string write(const std::string& name, const std::string& text) const {
        const auto p = dir / name;
        std::ofstream(p, std::ios::binary) << text;
        return p.string();
    }
};

}  // namespace

TEST(DiffusionComponents, SidecarWithParentTraversalIsRejected) {
    TempDir t;
    const std::string model = t.write("m.gguf", "x");
    t.write("m.components.json", R"({"llm": "../outside.gguf"})");
    JobQueue queue;
    DiffusionImageEngine engine(queue);
    LoadRequest lr;
    lr.model_path = model;
    lr.backend = Backend::Cpu;
    auto r = engine.load(lr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation) << r.error().message;
    queue.shutdown();
}

TEST(DiffusionComponents, SidecarWithAbsolutePathIsRejected) {
    TempDir t;
    const std::string model = t.write("m.gguf", "x");
    t.write("m.components.json", R"({"vae": "C:/Windows/notepad.exe"})");
    JobQueue queue;
    DiffusionImageEngine engine(queue);
    LoadRequest lr;
    lr.model_path = model;
    lr.backend = Backend::Cpu;
    auto r = engine.load(lr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation) << r.error().message;
    queue.shutdown();
}

TEST(DiffusionComponents, SidecarWithMissingFileIsNotFound) {
    TempDir t;
    const std::string model = t.write("m.gguf", "x");
    t.write("m.components.json", R"({"llm": "absent.gguf"})");
    JobQueue queue;
    DiffusionImageEngine engine(queue);
    LoadRequest lr;
    lr.model_path = model;
    lr.backend = Backend::Cpu;
    auto r = engine.load(lr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound) << r.error().message;
    queue.shutdown();
}

TEST(DiffusionComponents, MalformedSidecarIsInvalidArgument) {
    TempDir t;
    const std::string model = t.write("m.gguf", "x");
    t.write("m.components.json", "{not json");
    JobQueue queue;
    DiffusionImageEngine engine(queue);
    LoadRequest lr;
    lr.model_path = model;
    lr.backend = Backend::Cpu;
    auto r = engine.load(lr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << r.error().message;
    queue.shutdown();
}

TEST(DiffusionComponents, UnknownParamsBackendIsInvalidArgument) {
    TempDir t;
    const std::string model = t.write("m.gguf", "x");
    JobQueue queue;
    DiffusionImageEngine engine(queue);
    LoadRequest lr;
    lr.model_path = model;
    lr.backend = Backend::Cpu;
    lr.params_backend = "vulkan0";
    auto r = engine.load(lr);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << r.error().message;
    queue.shutdown();
}

TEST(DiffusionPreset, UnknownPresetNameIsRejectedEvenWithoutAModel) {
    JobQueue queue;
    DiffusionImageEngine engine(queue);
    ImageRequest q = small_request();
    q.preset = "turbo";
    auto r = engine.generate_image(q, CancellationToken{});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << r.error().message;
    queue.shutdown();
}

// SD1.5 has no measured preset table: asking for one must fail loudly, never fall back to the request's
// steps.
TEST(DiffusionPreset, PresetWithoutAFamilyTableIsAnErrorOnSd15) {
    REQUIRE_MODEL_OR_SKIP(path);
    Loaded m(path);
    ImageRequest q = small_request();
    q.preset = "fast";
    auto r = m.engine.generate_image(q, CancellationToken{});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << r.error().message;
}

// Real FLUX.3 Klein (diffusion GGUF + sidecar to the Qwen3-VL encoder and VAE). NOT RUN without the files.
TEST(DiffusionPreset, Flux3PresetsReplaceStepsAndReportThem) {
    const std::string path = env("HCS_TEST_FLUX3_MODEL");
    if (path.empty())
        GTEST_SKIP() << "NOT RUN: HCS_TEST_FLUX3_MODEL is not set (needs the FLUX.3 Klein GGUF + components)";
    JobQueue queue;
    DiffusionImageEngine engine(queue);
    LoadRequest lr;
    lr.model_path = path;
    lr.backend = Backend::Cpu;
    ASSERT_TRUE(engine.load(lr));
    ImageRequest q = small_request();
    q.steps = 2;
    q.preset = "quality";
    auto r = engine.generate_image(q, CancellationToken{});
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().steps, 16u);
    EXPECT_EQ(r.value().preset, "quality");
    // The measured `fast` point (docs/test-evidence/wave-f/README.md): 6 steps.
    q.preset = "fast";
    auto f = engine.generate_image(q, CancellationToken{});
    ASSERT_TRUE(f) << f.error().message;
    EXPECT_EQ(f.value().steps, 6u);
    EXPECT_EQ(f.value().preset, "fast");
    (void)engine.unload();
    queue.shutdown();
}
