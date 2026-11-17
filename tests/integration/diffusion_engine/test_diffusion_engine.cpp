// Integration tests for the stable-diffusion.cpp adapter through the BLOCKING entry points used by the
// HTTP/MCP image routes (IImageStreamEngine). Real model required: set HCS_TEST_DIFFUSION_MODEL=<small
// diffusion GGUF>. Output is verified structurally (PNG signature, size); image quality is not judged.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
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
