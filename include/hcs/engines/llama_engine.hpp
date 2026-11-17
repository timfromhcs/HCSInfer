#pragma once
// Text engine adapter over llama.cpp (AGENTS.md 3.2). This header is deliberately free of llama.h /
// ggml.h: upstream types live only in src/engines/llama.
//
// Threading contract:
//  - load()/unload()/generate()/metrics() may be called from any thread.
//  - Exactly one inference context exists; generation jobs run on the JobQueue worker(s) and are
//    serialised on that context (one owner).
//  - unload() cancels queued/running jobs of this engine, waits for them, then frees the context and
//    model (Vulkan buffers included) before returning.
//  - The JobQueue passed to the constructor must outlive the engine.
#include <memory>
#include <string>
#include <vector>

#include "hcs/engine.hpp"
#include "hcs/job_queue.hpp"

namespace hcs::engines {

struct GpuDeviceInfo {
    std::string name;         // ggml device name, e.g. "Vulkan0"
    std::string description;  // e.g. "AMD Radeon(TM) Graphics"
    std::uint64_t memory_total_bytes = 0;
    std::uint64_t memory_free_bytes = 0;
};

class LlamaTextEngine final : public ITextEngine {
public:
    explicit LlamaTextEngine(JobQueue& queue);
    ~LlamaTextEngine() override;
    LlamaTextEngine(const LlamaTextEngine&) = delete;
    LlamaTextEngine& operator=(const LlamaTextEngine&) = delete;

    // Backend selection: Cpu => no GPU device is used (ngl 0, KV on host); Vulkan => requires a
    // Vulkan-capable build AND a GPU device, otherwise Unsupported/BackendError (no silent fallback);
    // Auto => Vulkan when a GPU device exists, else CPU. A failed load leaves the engine unloaded.
    Result<void> load(const LoadRequest&) override;
    Result<void> unload() override;

    // Streams output chunks as JobEventKind::Output events of the returned job. Cancellation via
    // JobQueue::cancel(); a cancelled job ends in JobState::Cancelled. Applies the model's chat template
    // (system_prompt + prompt as user message) unless apply_chat_template is false.
    Result<JobId> generate(const TextRequest&) override;
    Result<JobId> generate(const TextRequest&, EventCallback on_event);

    // Image understanding through libmtmd. Requires load() to have been given LoadRequest::projector_path
    // (the model's mmproj GGUF); otherwise Unsupported. The image is decoded in-process (png/jpeg/bmp/gif,
    // no video/audio); inputs above kMaxVisionImageBytes / kMaxVisionImagePixels are InvalidArgument.
    // Output streams as JobEventKind::Output events exactly like generate(). The prompt must not contain
    // the multimodal marker literal (it is inserted by the adapter).
    Result<JobId> analyze_image(const VisionRequest&) override;
    Result<JobId> analyze_image(const VisionRequest&, EventCallback on_event);
    // True iff the loaded model has a working vision projector.
    bool vision_loaded() const;
    static constexpr std::size_t kMaxVisionImageBytes = 32u << 20;
    static constexpr std::uint64_t kMaxVisionImagePixels = 16u << 20;

    EngineMetrics metrics() const override;
    bool loaded() const;

    // Upstream log text captured during the most recent load() (backend/device evidence), bounded.
    std::string last_load_log() const;

    // True iff llama.cpp was built with the Vulkan backend into this binary.
    static bool vulkan_compiled();
    // GPU devices visible to the linked ggml backends (empty on CPU-only builds or without driver).
    static std::vector<GpuDeviceInfo> gpu_devices();
    // Upstream build identification (llama.cpp commit string).
    static std::string upstream_build_info();

private:
    Result<void> load_impl(const LoadRequest&);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hcs::engines
