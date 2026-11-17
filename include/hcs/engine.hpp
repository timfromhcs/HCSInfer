#pragma once
// Stable HCS-owned engine interfaces (AGENTS.md 3.2). Opaque value types only: no llama_*,
// ggml_* or sd_* types may appear here. Upstream-specific code lives in engine adapters
// (src/engines/llama, src/engines/diffusion) that implement these interfaces.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/types.hpp"

namespace hcs {

enum class Backend { Auto, Cpu, Vulkan };

struct ModelInfo {
    std::string id;
    std::string path;
    std::string architecture = "unknown";
    std::string quantization = "unknown";
    std::uint64_t size_bytes = 0;
    bool supports_vision = false;
};

struct LoadRequest {
    std::string model_id;
    std::string model_path;  // already resolved through PathGuard by the caller
    Backend backend = Backend::Auto;
    std::string device;            // empty => engine default
    std::int32_t gpu_layers = -1;  // -1 => engine default
    std::uint32_t context_tokens = 4096;
    std::int32_t threads = 0;    // 0 => auto
    std::string projector_path;  // optional mmproj for vision
    // Weight loading mode, engine-interpreted: "" / "auto" (engine default), "mmap", "none" (read into
    // RAM, no mmap), "mlock", "mmap+mlock", "direct_io". Unknown values => InvalidArgument.
    std::string load_mode;
};

struct TextRequest {
    std::string prompt;  // never logged
    std::uint32_t max_tokens = 256;
    float temperature = 0.8f;
    std::optional<std::uint64_t> seed;
    std::string system_prompt;        // optional, never logged
    bool apply_chat_template = true;  // false => `prompt` is tokenized verbatim
};

struct ImageInput {
    std::vector<std::uint8_t> bytes;  // encoded image (png/jpeg); decoded-pixel limits enforced by caller
};

struct VisionRequest {
    TextRequest text;
    ImageInput image;
};

struct ImageRequest {
    std::string prompt;
    std::string negative_prompt;
    std::uint32_t width = 512;
    std::uint32_t height = 512;
    std::uint32_t steps = 20;
    std::optional<std::uint64_t> seed;
    std::optional<ImageInput> init_image;  // edit / img2img
    float strength = 0.75f;                // denoising strength for edit / img2img (0..1)
    float cfg_scale = 0.0f;                // 0 => engine/model default (text CFG)
};

struct EngineMetrics {
    std::string backend = "unknown";  // "cpu" | "vulkan" | "unknown"
    std::string device = "unknown";
    double load_ms = 0.0;
    double ttft_ms = 0.0;
    double tokens_per_second = 0.0;
    std::uint64_t resident_bytes = 0;
    // Added by the llama adapter (M2). Zero/-1 = not measured / not applicable.
    std::uint64_t prompt_tokens = 0;
    double prompt_tokens_per_second = 0.0;
    std::uint64_t decode_tokens = 0;
    double decode_tokens_per_second = 0.0;  // == tokens_per_second for text engines
    std::int32_t offloaded_layers = -1;
    std::int32_t total_layers = -1;
    // Added by the diffusion adapter (M2). Zero = not measured / not applicable.
    double image_generation_ms = 0.0;
    std::uint32_t image_steps = 0;
};

class ITextEngine {
public:
    virtual ~ITextEngine() = default;
    virtual Result<void> load(const LoadRequest&) = 0;
    virtual Result<void> unload() = 0;
    virtual Result<JobId> generate(const TextRequest&) = 0;
    virtual Result<JobId> analyze_image(const VisionRequest&) = 0;
    virtual EngineMetrics metrics() const = 0;
};

class IImageEngine {
public:
    virtual ~IImageEngine() = default;
    virtual Result<void> load(const LoadRequest&) = 0;
    virtual Result<void> unload() = 0;
    virtual Result<JobId> generate(const ImageRequest&) = 0;
    virtual Result<JobId> edit(const ImageRequest&) = 0;
    virtual EngineMetrics metrics() const = 0;
};

}  // namespace hcs
