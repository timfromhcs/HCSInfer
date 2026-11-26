#pragma once
// HCS-owned Vulkan kernels ported from hcsuda @94a3485 (see docs/hcsuda-port.md): tiled f32/f16 GEMM,
// int8-dot GEMM, cooperative-matrix GEMM, softmax, LayerNorm, RMSNorm, RoPE. Every kernel has a CPU oracle
// (hcs/kernels/oracle.hpp) and is feature-gated by what the device reports (hardware::gate_kernel).
//
// NOTHING here is on the model/engine path and nothing is enabled by default: the planner
// (hcs/hardware/planner.hpp) only routes work to a kernel after `HCSInfer tune` proved it.
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hcs/hardware/planner.hpp"
#include "hcs/kernels/vk_runner.hpp"

namespace hcs::kernels::vulkan {

// kernel id -> FNV-1a/64 hex of the SPIR-V embedded in this binary (the planner's `KernelHashes`).
hardware::KernelHashes kernel_hashes();

// A kernel with its inputs resident on the device, ready to be dispatched repeatedly (timing loops) and read
// back. Move-only; holds its buffers (and keeps the device alive).
class PreparedOp {
public:
    PreparedOp() = default;
    PreparedOp(PreparedOp&&) noexcept = default;
    PreparedOp& operator=(PreparedOp&&) noexcept = default;
    PreparedOp(const PreparedOp&) = delete;
    PreparedOp& operator=(const PreparedOp&) = delete;

    Result<DispatchTiming> run();                            // one dispatch, data resident
    Result<void> read_output(void* dst, std::size_t bytes);  // bytes <= output_bytes()
    std::size_t output_bytes() const { return output_bytes_; }
    double flops() const { return flops_; }  // theoretical work of one dispatch
    const std::string& kernel_id() const { return id_; }

private:
    friend class KernelSet;
    Runner* runner_ = nullptr;
    const Pipeline* pipeline_ = nullptr;
    std::vector<Buffer> buffers_;  // last one is the output
    std::vector<unsigned char> push_;
    std::uint32_t gx_ = 1, gy_ = 1, gz_ = 1;
    std::size_t output_bytes_ = 0;
    double flops_ = 0.0;
    std::string id_;
};

// Pipelines for one Runner (created lazily, cached). Must not outlive the Runner it was built for.
class KernelSet {
public:
    explicit KernelSet(Runner& runner) : runner_(&runner) {}

    // The device gate (capabilities + shape) for a kernel id; ineligible kernels are never created.
    hardware::Gate gate(const std::string& id, const hardware::Shape* shape = nullptr) const;

    // All inputs are in the math layout (row-major); packing for the device is done here.
    Result<PreparedOp> prepare_gemm_f32(const float* a, const float* b, std::uint32_t m, std::uint32_t n,
                                        std::uint32_t k);
    // a: m x k and b: k x n IEEE binary16 values (uint16_t). f32 math.
    Result<PreparedOp> prepare_gemm_f16(const std::uint16_t* a, const std::uint16_t* b, std::uint32_t m,
                                        std::uint32_t n, std::uint32_t k);
    // a: m x k int8, b: k x n int8, int32 result; k % 4 == 0.
    Result<PreparedOp> prepare_gemm_i8dot(const std::int8_t* a, const std::int8_t* b, std::uint32_t m,
                                          std::uint32_t n, std::uint32_t k);
    // f16 inputs, f32 accumulate; m, n, k multiples of 16.
    Result<PreparedOp> prepare_gemm_coopmat_f16(const std::uint16_t* a, const std::uint16_t* b,
                                                std::uint32_t m, std::uint32_t n, std::uint32_t k);
    Result<PreparedOp> prepare_softmax(const float* x, std::uint32_t rows, std::uint32_t cols);
    Result<PreparedOp> prepare_layernorm(const float* x, const float* gamma, const float* beta,
                                         std::uint32_t rows, std::uint32_t cols, float eps);
    Result<PreparedOp> prepare_rmsnorm(const float* x, const float* gamma, std::uint32_t rows,
                                       std::uint32_t cols, float eps);
    Result<PreparedOp> prepare_rope(const float* x, std::uint32_t rows, std::uint32_t dim, std::uint32_t pos0,
                                    float base);

private:
    Result<const Pipeline*> pipeline(const std::string& id, std::uint32_t num_buffers,
                                     std::uint32_t push_bytes);
    Result<PreparedOp> build(const std::string& id, std::vector<std::pair<const void*, std::size_t>> inputs,
                             std::size_t output_bytes, const void* push, std::uint32_t push_bytes,
                             std::uint32_t gx, std::uint32_t gy, double flops);
    Runner* runner_;
    std::map<std::string, Pipeline> pipelines_;
};

}  // namespace hcs::kernels::vulkan
