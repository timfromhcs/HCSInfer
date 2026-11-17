#pragma once
// Deterministic FakeImageEngine for CONTRACT tests only (AGENTS.md 16.2): it exercises the HTTP/MCP image
// plumbing (validation, job, cancellation, PNG transport, lifecycle). It is NOT evidence of image generation;
// the real stable-diffusion.cpp adapter is exercised by tests/e2e/image_real.
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "hcs/api/image_stream.hpp"
#include "test_png.hpp"

namespace hcs::test {

class FakeImageEngine final : public api::IImageStreamEngine {
public:
    int delay_ms = 0;               // per 10 ms slice, rendering takes `slices` slices
    int slices = 1;
    bool run_until_cancel = false;  // renders forever, only stops on cancel
    bool fail_load = false;
    std::atomic<int> loads{0}, unloads{0}, generations{0}, edits{0};
    std::atomic<bool> rendering{false};
    std::mutex mu;
    ImageRequest last;

    Result<void> load(const LoadRequest&) override {
        if (fail_load) return make_error(ErrorCode::ModelError, "fake image load failure");
        ++loads;
        return {};
    }
    Result<void> unload() override {
        ++unloads;
        return {};
    }
    Result<JobId> generate(const ImageRequest&) override { return make_error(ErrorCode::Unsupported, "fake: blocking only"); }
    Result<JobId> edit(const ImageRequest&) override { return make_error(ErrorCode::Unsupported, "fake: blocking only"); }
    EngineMetrics metrics() const override {
        EngineMetrics m;
        m.backend = "fake";
        m.device = "none";
        return m;
    }

    Result<api::ImageResult> generate_image(const ImageRequest& r, const CancellationToken& t) override {
        ++generations;
        return render(r, t);
    }
    Result<api::ImageResult> edit_image(const ImageRequest& r, const CancellationToken& t) override {
        ++edits;
        return render(r, t);
    }

private:
    Result<api::ImageResult> render(const ImageRequest& r, const CancellationToken& t) {
        {
            std::lock_guard lk(mu);
            last = r;
        }
        rendering = true;
        for (int i = 0; run_until_cancel || i < slices; ++i) {
            if (t.cancelled()) {
                rendering = false;
                return make_error(ErrorCode::Cancelled, "fake render cancelled");
            }
            if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            else if (run_until_cancel) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        rendering = false;
        api::ImageResult out;
        out.bytes = make_red_square_png(64);
        out.width = r.width;
        out.height = r.height;
        out.steps = r.steps;
        out.generation_ms = 1.0;
        return out;
    }
};

}  // namespace hcs::test
