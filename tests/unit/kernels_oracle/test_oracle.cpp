// CPU oracle (ported from the hcsuda reference tests) and the multi-threaded baseline.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "hcs/kernels/cpu.hpp"
#include "hcs/kernels/cpu_baseline.hpp"
#include "hcs/kernels/oracle.hpp"

using namespace hcs::kernels::cpu;

namespace {

std::vector<float> rand_f32(std::size_t n, std::uint32_t seed, float lo = -1.0f, float hi = 1.0f) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> d(lo, hi);
    std::vector<float> v(n);
    for (auto& x : v) x = d(g);
    return v;
}
std::vector<std::int8_t> rand_i8(std::size_t n, std::uint32_t seed) {
    std::mt19937 g(seed);
    std::uniform_int_distribution<int> d(-128, 127);
    std::vector<std::int8_t> v(n);
    for (auto& x : v) x = static_cast<std::int8_t>(d(g));
    return v;
}

}  // namespace

TEST(OracleGemm, MatchesHandComputation) {
    // [[1,2],[3,4]] * [[5,6],[7,8]] = [[19,22],[43,50]]
    const float a[] = {1, 2, 3, 4}, b[] = {5, 6, 7, 8};
    float c[4];
    gemm_f32(a, b, c, 2, 2, 2);
    EXPECT_EQ(c[0], 19.0f);
    EXPECT_EQ(c[1], 22.0f);
    EXPECT_EQ(c[2], 43.0f);
    EXPECT_EQ(c[3], 50.0f);
}

TEST(OracleGemm, NonSquareAndDegenerateShapes) {
    // 1 x 3 times 3 x 1 = scalar
    const float a[] = {1, 2, 3}, b[] = {4, 5, 6};
    float c[1];
    gemm_f32(a, b, c, 1, 1, 3);
    EXPECT_EQ(c[0], 32.0f);
    // k == 0 leaves zeros
    float z[4] = {9, 9, 9, 9};
    gemm_f32(nullptr, nullptr, z, 2, 2, 0);
    for (float v : z) EXPECT_EQ(v, 0.0f);
}

TEST(OracleGemm, I8IsExactAndHandlesExtremes) {
    const std::int8_t a[] = {1, 2, 3, 4}, b[] = {5, 6, 7, 8};
    std::int32_t c[4];
    gemm_i8(a, b, c, 2, 2, 2);
    EXPECT_EQ(c[0], 19);
    EXPECT_EQ(c[3], 50);
    // -128 * -128 * k stays exact in int32 for realistic k
    std::vector<std::int8_t> big(4096, static_cast<std::int8_t>(-128));
    std::int32_t one = 0;
    gemm_i8(big.data(), big.data(), &one, 1, 1, 4096);
    EXPECT_EQ(one, 128 * 128 * 4096);
}

TEST(OracleGemm, F16WidensExactlyThenAccumulatesInF32) {
    // 1.0 = 0x3C00, 2.0 = 0x4000, 0.5 = 0x3800
    const std::uint16_t a[] = {0x3C00, 0x4000}, b[] = {0x3800, 0x3C00};
    float c[1];
    gemm_f16(a, b, c, 1, 1, 2);
    EXPECT_EQ(c[0], 1.0f * 0.5f + 2.0f * 1.0f);
}

TEST(OracleSoftmax, SumsToOneIsStableAndUniformOnDegenerateRows) {
    const float x[] = {1000.0f, 1001.0f, 1002.0f};
    float y[3];
    softmax_rows(x, y, 1, 3);
    EXPECT_NEAR(y[0] + y[1] + y[2], 1.0f, 1e-6f);
    EXPECT_GT(y[2], y[1]);
    EXPECT_GT(y[1], y[0]);
    // All -inf: exp-sum is NaN/0 -> uniform (hcsuda behaviour, documented)
    const float inf = std::numeric_limits<float>::infinity();
    const float d[] = {-inf, -inf};
    float yd[2];
    softmax_rows(d, yd, 1, 2);
    EXPECT_EQ(yd[0], 0.5f);
    EXPECT_EQ(yd[1], 0.5f);
    softmax_rows(x, y, 3, 0);  // cols == 0 is a no-op
}

TEST(OracleNorm, LayerNormHasZeroMeanUnitVariance) {
    const float x[] = {1, 2, 3, 4}, g[] = {1, 1, 1, 1}, b[] = {0, 0, 0, 0};
    float y[4];
    layernorm_rows(x, g, b, y, 1, 4, 1e-5f);
    double mean = 0, var = 0;
    for (float v : y) mean += v;
    mean /= 4;
    for (float v : y) var += (v - mean) * (v - mean);
    var /= 4;
    EXPECT_NEAR(mean, 0.0, 1e-6);
    EXPECT_NEAR(var, 1.0, 1e-3);
}

TEST(OracleNorm, RmsNormPreservesDirection) {
    const float x[] = {3, 4}, g[] = {1, 1};
    float y[2];
    rmsnorm_rows(x, g, y, 1, 2, 0.0f);
    const float rms = std::sqrt(12.5f);
    EXPECT_NEAR(y[0], 3.0f / rms, 1e-6f);
    EXPECT_NEAR(y[1], 4.0f / rms, 1e-6f);
}

TEST(OracleRope, PositionZeroIsIdentityAndRotationPreservesNorm) {
    const float x[] = {1, 2, 3, 4};  // dim 4, one row at position 0
    float y[4];
    rope_rows(x, y, 1, 4, 0, 10000.0f);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(y[i], x[i]);
    // Position 5: pair (x0,x2) rotates by theta = 5 * base^0 = 5 rad
    rope_rows(x, y, 1, 4, 5, 10000.0f);
    EXPECT_NEAR(y[0], 1.0f * std::cos(5.0f) - 3.0f * std::sin(5.0f), 1e-6f);
    EXPECT_NEAR(y[2], 1.0f * std::sin(5.0f) + 3.0f * std::cos(5.0f), 1e-6f);
    // pair (x1,x3): theta = 5 * 10000^(-2/4) = 0.05
    EXPECT_NEAR(y[1], 2.0f * std::cos(0.05f) - 4.0f * std::sin(0.05f), 1e-6f);
    double n_in = 0, n_out = 0;
    for (int i = 0; i < 4; ++i) {
        n_in += x[i] * x[i];
        n_out += y[i] * y[i];
    }
    EXPECT_NEAR(n_in, n_out, 1e-5);
}

TEST(OracleTolerance, ScalesWithReductionLengthAndHandlesNanInf) {
    const Tolerance t = tolerance_f32();
    EXPECT_GT(t.for_len(4096).abs, t.abs);
    EXPECT_DOUBLE_EQ(t.for_len(0).abs, t.abs);  // len 0 treated as 1
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_TRUE(t.accepts(nan, nan));
    EXPECT_FALSE(t.accepts(nan, 1.0f));
    EXPECT_TRUE(t.accepts(inf, inf));
    EXPECT_FALSE(t.accepts(inf, -inf));
    EXPECT_TRUE(t.accepts(1.0f, 1.0f + 5e-6f));
    EXPECT_FALSE(t.accepts(1.0f, 1.0f + 1e-3f));
}

TEST(OracleCompare, ReportsWorstIndexAndMismatchCount) {
    const float want[] = {1, 2, 3, 4}, got[] = {1, 2, 3.5f, 4};
    const Comparison c = compare(want, got, 4, tolerance_f32());
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(c.mismatches, 1u);
    EXPECT_EQ(c.worst_index, 2u);
    EXPECT_NEAR(c.max_abs_error, 0.5, 1e-9);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float wn[] = {1.0f}, gn[] = {nan};
    EXPECT_FALSE(compare(wn, gn, 1, tolerance_f32()).ok);
    const std::int32_t a[] = {1, 2, 3}, b[] = {1, 9, 3};
    EXPECT_FALSE(compare_exact(a, b, 3).ok);
    EXPECT_TRUE(compare_exact(a, a, 3).ok);
}

// --- baseline == oracle (odd / tail / non-power-of-two shapes, threads forced to exercise splitting) ----

TEST(CpuBaseline, GemmF32AgreesWithOracle) {
    struct S {
        std::size_t m, n, k;
    };
    for (const S s : {S{67, 131, 95}, S{1, 1, 1}, S{5, 1, 33}, S{1, 257, 3}, S{130, 65, 17}}) {
        const std::size_t m = s.m, n = s.n, k = s.k;
        const auto a = rand_f32(m * k, 1), b = rand_f32(k * n, 2);
        std::vector<float> want(m * n), got(m * n);
        gemm_f32(a.data(), b.data(), want.data(), m, n, k);
        baseline_gemm_f32(a.data(), b.data(), got.data(), m, n, k, 4);
        const auto cmp = compare(want.data(), got.data(), m * n, tolerance_f32().for_len(k));
        EXPECT_TRUE(cmp.ok) << m << "x" << n << "x" << k << " max err " << cmp.max_abs_error;
    }
}

TEST(CpuBaseline, GemmI8IsBitEqualToOracle) {
    const std::size_t m = 37, n = 29, k = 132;
    const auto a = rand_i8(m * k, 3), b = rand_i8(k * n, 4);
    std::vector<std::int32_t> want(m * n), got(m * n);
    gemm_i8(a.data(), b.data(), want.data(), m, n, k);
    baseline_gemm_i8(a.data(), b.data(), got.data(), m, n, k, 3);
    EXPECT_TRUE(compare_exact(want.data(), got.data(), m * n).ok);
}

TEST(CpuBaseline, RowOpsAgreeWithOracle) {
    const std::size_t rows = 33, cols = 257;
    const auto x = rand_f32(rows * cols, 5, -4.0f, 4.0f), g = rand_f32(cols, 6), b = rand_f32(cols, 7);
    std::vector<float> w(rows * cols), o(rows * cols);

    softmax_rows(x.data(), w.data(), rows, cols);
    baseline_softmax_rows(x.data(), o.data(), rows, cols, 4);
    EXPECT_TRUE(compare(w.data(), o.data(), w.size(), tolerance_f32().for_len(cols)).ok);

    layernorm_rows(x.data(), g.data(), b.data(), w.data(), rows, cols, 1e-5f);
    baseline_layernorm_rows(x.data(), g.data(), b.data(), o.data(), rows, cols, 1e-5f, 4);
    EXPECT_TRUE(compare(w.data(), o.data(), w.size(), tolerance_f32().for_len(cols)).ok);

    rmsnorm_rows(x.data(), g.data(), w.data(), rows, cols, 1e-5f);
    baseline_rmsnorm_rows(x.data(), g.data(), o.data(), rows, cols, 1e-5f, 4);
    EXPECT_TRUE(compare(w.data(), o.data(), w.size(), tolerance_f32().for_len(cols)).ok);

    const std::size_t dim = 126;  // even, not a power of two
    const auto xr = rand_f32(40 * dim, 8);
    std::vector<float> wr(40 * dim), orr(40 * dim);
    rope_rows(xr.data(), wr.data(), 40, dim, 3, 10000.0f);
    baseline_rope_rows(xr.data(), orr.data(), 40, dim, 3, 10000.0f, 4);
    // float sin/cos of angles up to ~42 rad: 2e-4 absolute is the declared f32 rope tolerance
    EXPECT_TRUE(compare(wr.data(), orr.data(), wr.size(), Tolerance{1e-5, 2e-4}).ok);
}
