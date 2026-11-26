#pragma once
// CPU reference oracle for the HCS-owned Vulkan kernels (AGENTS.md 12.4: every kernel keeps an independent
// CPU reference). Ported from hcsuda @94a3485 (crates/hcsuda-core/src/reference.rs + dtype.rs tolerances),
// see docs/hcsuda-port.md.
//
// Deliberately simple and single-threaded. Accumulation is done in double precision and rounded once, so the
// oracle is the *true* value up to the final f32 rounding; a GPU kernel that sums in f32 in a different order
// is then compared against it with a tolerance that scales with the reduction length (see Tolerance).
//
// Layouts (row-major everywhere):
//   gemm:   C[m x n] = A[m x k] * B[k x n]
//   rows:   x[rows x cols], one independent row per output row
#include <cstddef>
#include <cstdint>

namespace hcs::kernels::cpu {

// --- tolerance -----------------------------------------------------------------------------------------
// accepts(want, got) <=> |want - got| <= abs + rel * |want|. `abs` grows with sqrt(reduction length) because a
// longer f32 sum accumulates more rounding (hcsuda DType::tolerance + Tolerance::for_len).
struct Tolerance {
    double rel = 0.0;
    double abs = 0.0;

    Tolerance for_len(std::size_t len) const;
    bool accepts(float want, float got) const;
};

Tolerance tolerance_f32();  // rel 1e-5, abs 1e-6 (per sqrt(len))
Tolerance tolerance_f16();  // rel 1e-2, abs 1e-3 (documented for narrow formats; the f16 GEMM stores halves
                            // but widens to f32 before the math, so it is checked with tolerance_f32())
Tolerance tolerance_exact();  // integer kernels must be bit-equal

struct Comparison {
    bool ok = true;
    double max_abs_error = 0.0;
    std::size_t worst_index = 0;
    std::size_t mismatches = 0;  // elements outside tolerance (NaN only equals NaN, inf only equals inf)
    std::size_t count = 0;
};

Comparison compare(const float* want, const float* got, std::size_t n, const Tolerance& tol);
// Exact comparison for integer outputs.
Comparison compare_exact(const std::int32_t* want, const std::int32_t* got, std::size_t n);

// --- ops -----------------------------------------------------------------------------------------------
void gemm_f32(const float* a, const float* b, float* c, std::size_t m, std::size_t n, std::size_t k);

// A and B hold IEEE binary16 values (uint16_t); math is exact-widen-to-f32 then accumulate.
void gemm_f16(const std::uint16_t* a, const std::uint16_t* b, float* c, std::size_t m, std::size_t n,
              std::size_t k);

// Signed int8 inputs, int32 accumulation (exact). B is k x n here (the mathematical layout); the Vulkan
// wrapper transposes/packs it for the device.
void gemm_i8(const std::int8_t* a, const std::int8_t* b, std::int32_t* c, std::size_t m, std::size_t n,
             std::size_t k);

// Numerically stable softmax per row. A row whose exp-sum is zero or non-finite becomes uniform 1/cols.
void softmax_rows(const float* x, float* y, std::size_t rows, std::size_t cols);

void layernorm_rows(const float* x, const float* gamma, const float* beta, float* y, std::size_t rows,
                    std::size_t cols, float eps);

void rmsnorm_rows(const float* x, const float* gamma, float* y, std::size_t rows, std::size_t cols, float eps);

// Rotary position embedding, split-half layout: element i pairs with i + dim/2. Row r is token position
// pos0 + r; pair i rotates by pos * base^(-2i/dim). `dim` must be even.
void rope_rows(const float* x, float* y, std::size_t rows, std::size_t dim, std::uint32_t pos0, float base);

}  // namespace hcs::kernels::cpu
