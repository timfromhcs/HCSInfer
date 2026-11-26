#pragma once
// 4K upscaler pipeline (plan B4). Three layers, each testable on its own:
//
//  1. IUpscaleEngine - a narrow "run one tile through a super-resolution model" contract. The only
//     production implementation is the dynamically loaded ONNX Runtime backend (hcs/upscale_ort.hpp). Nothing
//     in this header depends on ONNX Runtime, stable-diffusion.cpp or ggml.
//  2. The tiled pipeline: tile size and overlap come from the memory budget, tiles are blended with a linear
//     feather so seams vanish, the model pass runs at the model's native scale (4x) and a Lanczos-3 resample
//     brings the result to EXACTLY `target_long_edge` (default 3840). An image whose long edge is already at or
//     above the target is never upscaled (skipped, reported as such).
//  3. PNG glue (decode -> pipeline -> encode) used by the control plane.
//
// Pixel formats: Rgb8 is interleaved 8-bit RGB; engine tiles are interleaved float32 RGB in [0,1].
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hcs/job_queue.hpp"
#include "hcs/result.hpp"

namespace hcs::upscale {

inline constexpr std::uint32_t kDefaultTargetLongEdge = 3840;
// When no live memory budget is known the planner assumes this much instead of "unlimited".
inline constexpr std::uint64_t kUnknownBudgetBytes = 1ull << 30;
inline constexpr std::uint64_t kMaxSourcePixels = 16ull << 20;  // refuse absurd inputs before allocating

struct ModelInfo {
    int scale = 4;                  // native up-scaling factor of the model (1..8)
    std::uint32_t fixed_tile = 0;   // >0: the model only accepts fixed_tile x fixed_tile inputs
    // Peak engine working memory per INPUT pixel (activations + arena). Conservative default; an engine may
    // report a measured figure.
    std::uint64_t working_bytes_per_input_pixel = 4096;
    std::string name;      // model file name
    std::string provider;  // execution provider actually in use ("DirectML" | "CPU" | test double name)
};

class IUpscaleEngine {
public:
    virtual ~IUpscaleEngine() = default;
    // Ok when the runtime and the model are present and loadable; otherwise a structured error (Unsupported
    // for a missing runtime/model) that names what is missing.
    virtual Result<void> ready() const = 0;
    virtual ModelInfo info() const = 0;
    // `in`: w*h*3 floats (HWC, RGB, 0..1). `out`: (w*scale)*(h*scale)*3 floats, same layout.
    virtual Result<void> run_tile(const float* in, std::uint32_t w, std::uint32_t h, float* out,
                                  const CancellationToken& token) = 0;
};

struct Rgb8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;  // width*height*3
};

struct Options {
    std::uint32_t target_long_edge = kDefaultTargetLongEdge;
    std::uint32_t tile = 0;     // 0 => chosen from the budget
    std::uint32_t overlap = 0;  // 0 => 16 (clamped to tile/4)
    std::uint64_t memory_budget_bytes = 0;  // 0 => kUnknownBudgetBytes
};

struct TileRect {
    std::uint32_t x = 0, y = 0, w = 0, h = 0;  // in source pixels
};

struct Plan {
    bool skip = false;  // source already at/above the target: nothing to do
    std::string skip_reason;
    std::uint32_t tile = 0;
    std::uint32_t overlap = 0;
    int scale = 4;
    std::uint32_t model_w = 0, model_h = 0;  // source * scale
    std::uint32_t out_w = 0, out_h = 0;      // final size
    std::vector<TileRect> tiles;
    std::uint64_t peak_bytes = 0;  // estimated peak memory of the whole pipeline
};

struct Stats {
    std::uint32_t tiles = 0;
    std::uint32_t tile = 0;
    std::uint32_t overlap = 0;
    std::uint32_t src_w = 0, src_h = 0, out_w = 0, out_h = 0;
    double model_ms = 0.0;
    double resample_ms = 0.0;
    std::uint64_t peak_bytes_estimate = 0;
    bool skipped = false;
    std::string skip_reason;
    std::string provider;
    std::string model;
};

// Tile rectangles covering w x h with the given stride (= tile - overlap); the last tile in each row/column is
// aligned to the image edge. A dimension not larger than `tile` yields a single tile of that dimension.
std::vector<TileRect> plan_tiles(std::uint32_t w, std::uint32_t h, std::uint32_t tile, std::uint32_t overlap);

// Final size for a source: long edge == target, aspect preserved (the short edge is rounded, min 1).
void target_size(std::uint32_t w, std::uint32_t h, std::uint32_t target_long_edge, std::uint32_t& out_w,
                 std::uint32_t& out_h);

// Peak-memory model of the pipeline for a given tile size (see tiling.cpp for the terms).
std::uint64_t estimate_peak_bytes(std::uint32_t src_w, std::uint32_t src_h, std::uint32_t tile,
                                  const ModelInfo& model, std::uint32_t out_w, std::uint32_t out_h);

Result<Plan> plan_upscale(std::uint32_t src_w, std::uint32_t src_h, const ModelInfo& model, const Options& opt);

// Feather weight of position `i` (0-based) in a tile edge ramp of length `ramp`: strictly positive, rises
// linearly to 1 at i >= ramp. Exposed for tests.
float feather_weight(std::uint32_t i, std::uint32_t ramp) noexcept;

// Lanczos-3 resample of an interleaved float image (any size) to out_w x out_h, rounded to 8 bit. Filter support
// scales with the downsampling ratio (no aliasing). Weights are normalised, so flat regions stay flat.
Rgb8 resample_lanczos(const float* rgb, std::uint32_t w, std::uint32_t h, std::uint32_t out_w,
                      std::uint32_t out_h);

using ProgressFn = std::function<void(double /*0..1*/)>;

// Runs the whole pipeline. On skip, returns the source unchanged and fills stats.skipped.
Result<Rgb8> upscale_image(IUpscaleEngine& engine, const Rgb8& src, const Options& opt,
                           const CancellationToken& token, const ProgressFn& progress, Stats* stats);

// PNG helpers (stb based). decode_png accepts 8-bit RGB/RGBA/gray PNG and drops alpha.
Result<Rgb8> decode_png(const std::uint8_t* data, std::size_t size);
Result<std::vector<std::uint8_t>> encode_png(const Rgb8& img);

// decode -> upscale -> encode. On skip the input bytes are returned unchanged.
Result<std::vector<std::uint8_t>> upscale_png(IUpscaleEngine& engine, const std::vector<std::uint8_t>& png,
                                              const Options& opt, const CancellationToken& token,
                                              const ProgressFn& progress, Stats* stats);

}  // namespace hcs::upscale
