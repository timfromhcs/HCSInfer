#pragma once
// Multi-threaded, cache-friendly CPU implementations of the same ops as oracle.hpp. This is the *speed*
// baseline the tuner compares the Vulkan kernels against (the single-threaded double-precision oracle is a
// correctness reference and a weak speed baseline: hcsuda docs/MEASURED.md "13-38x says nothing about a tuned
// multi-threaded BLAS"). Float accumulation, row-parallel via std::thread, no external BLAS, no SIMD
// intrinsics: the compiler's auto-vectorisation is all the vectorisation it gets, so it is a modest baseline
// and is reported as such ("cpu_mt_autovec"), never as "the CPU's best".
#include <cstddef>
#include <cstdint>

namespace hcs::kernels::cpu {

// threads == 0 -> std::thread::hardware_concurrency(). Work is only split when it is large enough to
// amortise thread start-up; tiny problems run inline (as production code would).
void baseline_gemm_f32(const float* a, const float* b, float* c, std::size_t m, std::size_t n, std::size_t k,
                       unsigned threads = 0);
void baseline_gemm_i8(const std::int8_t* a, const std::int8_t* b, std::int32_t* c, std::size_t m,
                      std::size_t n, std::size_t k, unsigned threads = 0);
void baseline_softmax_rows(const float* x, float* y, std::size_t rows, std::size_t cols, unsigned threads = 0);
void baseline_layernorm_rows(const float* x, const float* gamma, const float* beta, float* y, std::size_t rows,
                             std::size_t cols, float eps, unsigned threads = 0);
void baseline_rmsnorm_rows(const float* x, const float* gamma, float* y, std::size_t rows, std::size_t cols,
                           float eps, unsigned threads = 0);
void baseline_rope_rows(const float* x, float* y, std::size_t rows, std::size_t dim, std::uint32_t pos0,
                        float base, unsigned threads = 0);

}  // namespace hcs::kernels::cpu
