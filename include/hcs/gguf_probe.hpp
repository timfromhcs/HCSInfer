#pragma once
// Bounded, allocation-safe GGUF header probe (AGENTS.md 19.3: model files are untrusted input).
//
// This is NOT a tensor loader and it never decodes weights. It reads the header, every metadata
// key/value pair and the tensor-info names so the registry can report a real architecture and make
// a defensible text/image classification. Every length and offset is bounds-checked against the
// bytes actually read, so a truncated or hostile file yields `valid == false` or
// `complete == false` instead of a crash, a huge allocation or an out-of-bounds read.
//
// Only GGUF v2/v3 are parsed (the pinned llama.cpp/stable-diffusion.cpp reject v1). A v1 file is
// reported as valid-but-incomplete so the existing "GGUF header is acceptable" contract is kept
// without pretending we understood its counts.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "hcs/image_profiles.hpp"
#include "hcs/model_registry.hpp"

namespace hcs {

struct GgufProbeResult {
    // `valid`: the magic and version were accepted (the file is recognisably GGUF).
    // `complete`: the whole declared metadata/tensor-info table was walked inside the probed bytes
    // with no bounds violation. A truncated file is valid-but-incomplete, never a crash.
    bool valid = false;
    bool complete = false;
    std::uint32_t version = 0;
    std::uint64_t tensor_count = 0;
    std::uint64_t kv_count = 0;
    std::size_t tensor_names_read = 0;
    std::string architecture;             // "general.architecture" when the file declares it
    ModelKind kind = ModelKind::Unknown;  // best-effort hint, refined by engine adapters
    // Structure of a diffusion checkpoint read from tensor names and shapes (never from the file name):
    // text-context width, SDXL label embedding, FLUX blocks. All zero/false for text models.
    ImageModelSignature diffusion;
    std::string reason;                  // why it is invalid or incomplete (empty when clean)
};

// Probes an in-memory prefix. `max_tensor_names` bounds how many tensor-info entries are walked.
GgufProbeResult probe_gguf(std::span<const std::uint8_t> bytes, std::size_t max_tensor_names = 4096);

// Reads at most `max_bytes` of the file (default 4 MiB) and probes that prefix. A missing or
// unreadable file yields valid == false with a reason; it never throws.
GgufProbeResult probe_gguf_file(const std::filesystem::path& path, std::size_t max_bytes = 4u << 20);

}  // namespace hcs

