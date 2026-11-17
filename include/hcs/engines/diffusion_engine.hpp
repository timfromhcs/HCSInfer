#pragma once
// Image engine adapter over stable-diffusion.cpp (AGENTS.md 3.2). stable-diffusion.h and any ggml
// types stay inside src/engines/diffusion; this header only exposes HCS value types.
//
// Threading contract (same model as the llama adapter):
//  - load()/unload()/generate()/metrics() may be called from any thread.
//  - Exactly one sd_ctx_t exists; jobs are serialised on that context's run mutex.
//  - unload() cancels and waits for queued/running jobs before freeing the context.
//  - The JobQueue passed to the constructor must outlive the engine. It is used by the
//    JobQueue-based generate()/edit() entry points; the blocking entry points do not touch it.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hcs/api/image_stream.hpp"
#include "hcs/engine.hpp"
#include "hcs/job_queue.hpp"

namespace hcs::engines {

class DiffusionImageEngine final : public api::IImageStreamEngine {
public:
    explicit DiffusionImageEngine(JobQueue& queue);
    ~DiffusionImageEngine() override;
    DiffusionImageEngine(const DiffusionImageEngine&) = delete;
    DiffusionImageEngine& operator=(const DiffusionImageEngine&) = delete;

    // Backend selection: Cpu => "CPU" device; Vulkan => first GPU device (must exist and the build
    // must have Vulkan); Auto => GPU when present, else CPU. No silent fallback for an explicit
    // request. Weights default to the host ("cpu" params backend) on the UMA iGPU - measured
    // configuration in docs/test-evidence/M1/sd.
    Result<void> load(const LoadRequest&) override;
    Result<void> unload() override;

    // JobQueue entry points: the job renders one image and stores it; retrieve it with
    // image_result(job_id) (NotFound until the job finished successfully).
    Result<JobId> generate(const ImageRequest&) override;
    Result<JobId> edit(const ImageRequest&) override;

    EngineMetrics metrics() const override;

    Result<api::ImageResult> generate_image(const ImageRequest&, const CancellationToken&) override;
    Result<api::ImageResult> edit_image(const ImageRequest&, const CancellationToken&) override;

    // Optional progress sink: fraction 0..1 plus a short human-readable stage message.
    using ProgressSink = std::function<void(double, std::string)>;
    Result<api::ImageResult> generate_image(const ImageRequest&, const CancellationToken&,
                                            const ProgressSink&);
    Result<api::ImageResult> edit_image(const ImageRequest&, const CancellationToken&, const ProgressSink&);

    Result<api::ImageResult> image_result(JobId) const;
    bool loaded() const;
    std::string last_load_log() const;

    // True iff this binary links a Vulkan-capable stable-diffusion.cpp/ggml build.
    static bool vulkan_compiled();
    // "NAME\tDESCRIPTION" lines as reported by sd_list_devices().
    static std::vector<std::string> devices();
    static std::string upstream_version();

private:
    Result<api::ImageResult> run(const ImageRequest&, const CancellationToken&, const ProgressSink&, bool require_init);
    Result<JobId> submit(const std::string& name, const ImageRequest&, bool require_init);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hcs::engines
