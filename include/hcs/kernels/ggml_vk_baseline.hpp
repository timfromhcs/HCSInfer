#pragma once
// In-process baseline: the pinned GGML Vulkan backend (the operators llama.cpp and stable-diffusion.cpp
// really run) driven directly with a one-node graph per operation, so HCS Vulkan kernels can be compared with
// the upstream operator on the same device, the same shape and the same timing method (AGENTS.md M8, 16.6).
//
// Method (identical for every operation): inputs are uploaded once and stay device resident; the graph is
// built once; `ggml_backend_graph_compute` (submit + wait) is timed per sample after a warm-up; the result is
// spot-checked against a double-precision reference so a wrong-but-fast operation can never be recorded.
// Nothing here changes any default.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/result.hpp"

namespace hcs::kernels::ggml_baseline {

// Weight (src0) type of ggml_mul_mat. Activations are F32 unless a variant says otherwise.
enum class WeightType { F32, F16, Q8_0, Q4_K };

std::string_view to_string(WeightType t) noexcept;

struct Measurement {
    std::string variant;  // what exactly was timed, e.g. "mul_mat f16 weights x f32 activations"
    std::uint32_t samples = 0;
    double median_ms = 0.0, p10_ms = 0.0, p90_ms = 0.0;
    double spread = 0.0;  // (p90 - p10) / median
    std::vector<double> raw_ms;
    double flops = 0.0;            // 0 = unknown
    double max_err_over_l1 = 0.0;  // spot check: |got - ref| / (sum |terms| + tiny); scale-free
    bool check_ok = false;
    std::string note;  // e.g. which of several upstream variants was the fastest
};

struct DeviceDescription {
    int ggml_index = -1;
    std::string description;  // as ggml reports it
    std::string backend;      // ggml_backend_name
    std::uint64_t memory_free = 0, memory_total = 0;
};

// Owns one ggml Vulkan backend. Not thread-safe; create one per benchmark run.
class Session {
public:
    // `device_name_substr` (case-insensitive) selects the ggml Vulkan device; empty = device 0.
    // Unsupported when this build has no ggml Vulkan backend or there is no device; NotFound when no name
    // matches.
    static Result<Session> create(std::string_view device_name_substr = {});
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    ~Session();

    const DeviceDescription& device() const;

    // ggml_mul_mat: C[m,n] = A[m,k] * W[n,k]^T (the same math as the HCS GEMM with B = W^T). For F16 weights
    // the faster of {F32 activations, F16 activations} is reported (the conservative baseline).
    Result<Measurement> mul_mat(WeightType w, std::uint32_t m, std::uint32_t n, std::uint32_t k,
                                std::uint32_t samples);
    Result<Measurement> softmax(std::uint32_t rows, std::uint32_t cols, std::uint32_t samples);
    // norm -> mul(gamma) -> add(beta): the op chain a LayerNorm model emits.
    Result<Measurement> layernorm(std::uint32_t rows, std::uint32_t cols, float eps, std::uint32_t samples);
    // rms_norm -> mul(gamma): the op chain a Llama-style model emits (GGML fuses it on Vulkan when it can).
    Result<Measurement> rmsnorm(std::uint32_t rows, std::uint32_t cols, float eps, std::uint32_t samples);
    // Split-half (NEOX) rotary embedding, base 10000, positions pos0 + row.
    Result<Measurement> rope(std::uint32_t rows, std::uint32_t dim, std::uint32_t pos0,
                             std::uint32_t samples);

private:
    struct Impl;
    explicit Session(std::unique_ptr<Impl> i);
    std::unique_ptr<Impl> impl_;
};

}  // namespace hcs::kernels::ggml_baseline
