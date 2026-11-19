#pragma once
// HCSInfer CPU reference kernels (AGENTS.md M8: every experimental Vulkan kernel needs an independent CPU
// reference implementation). These are deliberately simple, single-threaded and dependency-free so they can
// serve as the correctness oracle for the Vulkan kernels and as the dispatch fallback.
#include <cstdint>

namespace hcs::kernels::cpu {

// --- Q8_0 layout (matches ggml block_q8_0 so the kernels operate on real GGUF weights) -------------------
//   struct block_q8_0 { std::uint16_t d; std::int8_t qs[32]; };   // 34 bytes, value = fp16(d) * qs[i]
inline constexpr std::uint32_t kQ8_0BlockValues = 32;
inline constexpr std::uint32_t kQ8_0BlockBytes = 34;

// Number of Q8_0 blocks and bytes for one row of `k` columns (k must be a multiple of 32).
std::uint64_t q8_0_row_blocks(std::uint32_t k);
std::uint64_t q8_0_row_bytes(std::uint32_t k);
// Total bytes for an m x k Q8_0 matrix.
std::uint64_t q8_0_bytes(std::uint32_t m, std::uint32_t k);

// y[m] = sum_k dequant_q8_0(W[m, k]) * x[k].
//   x  : k F32 values
//   w  : raw Q8_0 bytes, row-major, m rows of k columns (m * q8_0_row_bytes(k) bytes)
//   y  : m F32 values (written)
// `k` must be a multiple of 32 and exactly `m * q8_0_row_bytes(k)` bytes must be readable from `w`.
void gemv_q8_0(const float* x, const void* w_q8_0, float* y, std::uint32_t m, std::uint32_t k);

// fp16 (IEEE 754 binary16, little-endian uint16) -> fp32. Also used by tests to build Q8_0 data.
float fp16_to_fp32(std::uint16_t h);
std::uint16_t fp32_to_fp16(float f);

}  // namespace hcs::kernels::cpu
