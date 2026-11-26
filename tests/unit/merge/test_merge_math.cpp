// Merge math against numpy golden values (tests/unit/merge/gen_merge_golden.py) and dtype conversions.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

#include "hcs/merge.hpp"

using namespace hcs::merge;

#include "merge_golden.inc"

namespace {
void expect_close(const std::vector<float>& got, const std::vector<float>& want, const char* what) {
    ASSERT_EQ(got.size(), want.size()) << what;
    for (std::size_t i = 0; i < got.size(); ++i)
        EXPECT_NEAR(got[i], want[i], 2e-6 + 1e-5 * std::fabs(want[i])) << what << " [" << i << "]";
}
}  // namespace

TEST(MergeMath, LinearMatchesNumpy) {
    std::vector<float> out(kGoldenN);
    linear_chunk({kBase.data(), kM1.data(), kM2.data()}, {0.5, 0.3, 0.2}, kGoldenN, out.data());
    expect_close(out, kLinear, "linear");
    // Weights are normalised: scaling them changes nothing.
    std::vector<float> out2(kGoldenN);
    linear_chunk({kBase.data(), kM1.data(), kM2.data()}, {5, 3, 2}, kGoldenN, out2.data());
    expect_close(out2, kLinear, "linear scaled weights");
}

TEST(MergeMath, SlerpMatchesNumpyAcrossChunks) {
    // One pass over everything ...
    SlerpStats s;
    slerp_accumulate(kBase.data(), kM1.data(), kGoldenN, s);
    std::vector<float> out(kGoldenN);
    slerp_apply(kBase.data(), kM1.data(), kGoldenN, slerp_coeffs(s, 0.3), out.data());
    expect_close(out, kSlerp, "slerp");
    // ... and with the statistics accumulated over uneven chunks (the streaming engine does this).
    SlerpStats c;
    slerp_accumulate(kBase.data(), kM1.data(), 7, c);
    slerp_accumulate(kBase.data() + 7, kM1.data() + 7, 19, c);
    slerp_accumulate(kBase.data() + 26, kM1.data() + 26, kGoldenN - 26, c);
    EXPECT_NEAR(c.dot, s.dot, 1e-9);
    EXPECT_NEAR(c.na, s.na, 1e-9);
    EXPECT_NEAR(c.nb, s.nb, 1e-9);
}

TEST(MergeMath, SlerpEdgeCases) {
    SlerpStats par;
    slerp_accumulate(kBase.data(), kPar.data(), kGoldenN, par);
    std::vector<float> out(kGoldenN);
    slerp_apply(kBase.data(), kPar.data(), kGoldenN, slerp_coeffs(par, 0.25), out.data());
    expect_close(out, kSlerpParallel, "parallel -> lerp");
    // t = 0 returns a, t = 1 returns b.
    SlerpStats s;
    slerp_accumulate(kBase.data(), kM1.data(), kGoldenN, s);
    slerp_apply(kBase.data(), kM1.data(), kGoldenN, slerp_coeffs(s, 0.0), out.data());
    expect_close(out, kBase, "t=0");
    slerp_apply(kBase.data(), kM1.data(), kGoldenN, slerp_coeffs(s, 1.0), out.data());
    expect_close(out, kM1, "t=1");
    // A zero vector has no direction: lerp, never NaN.
    std::vector<float> zero(kGoldenN, 0.0f);
    SlerpStats z;
    slerp_accumulate(zero.data(), kM1.data(), kGoldenN, z);
    const auto co = slerp_coeffs(z, 0.5);
    EXPECT_DOUBLE_EQ(co.ca, 0.5);
    EXPECT_DOUBLE_EQ(co.cb, 0.5);
}

TEST(MergeMath, TiesMatchesNumpy) {
    std::vector<float> scratch, thr;
    thr.push_back(ties_threshold(kM1.data(), kBase.data(), kGoldenN, 0.25, scratch));
    thr.push_back(ties_threshold(kM2.data(), kBase.data(), kGoldenN, 0.25, scratch));
    std::vector<float> out(kGoldenN);
    ties_chunk(kBase.data(), {kM1.data(), kM2.data()}, {1.0, 0.5}, thr, 0.8, kGoldenN, out.data());
    expect_close(out, kTies, "ties");
    // Exactly k = ceil(0.25 * 32) = 8 entries per task vector are kept: threshold is the 8th largest |delta|.
    std::vector<float> d(kGoldenN);
    for (std::size_t i = 0; i < kGoldenN; ++i) d[i] = std::fabs(kM1[i] - kBase[i]);
    std::sort(d.begin(), d.end(), std::greater<float>());
    EXPECT_NEAR(thr[0], d[7], 1e-6);
}

TEST(MergeMath, TiesDensityOneKeepsEverythingAndSignElectionDropsMinority) {
    const float base[3] = {0, 0, 0};
    const float a[3] = {1, 1, 1}, b[3] = {-0.5f, 1, -3};
    std::vector<float> thr = {0.0f, 0.0f};
    float out[3];
    ties_chunk(base, {a, b}, {1.0, 1.0}, thr, 1.0, 3, out);
    // idx0: sum 0.5 -> +, only a agrees -> 1; idx1: both agree -> mean 1; idx2: sum -2 -> -, only b -> -3.
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[1], 1.0f);
    EXPECT_FLOAT_EQ(out[2], -3.0f);
    // Exact tie: elected sign 0 => no change from the base.
    const float c[1] = {1}, d[1] = {-1}, z[1] = {0};
    float o1;
    ties_chunk(z, {c, d}, {1.0, 1.0}, thr, 1.0, 1, &o1);
    EXPECT_FLOAT_EQ(o1, 0.0f);
}

TEST(MergeMath, DareMatchesNumpyAndIsChunkIndependent) {
    const std::uint64_t key = fnv1a64("w.test");
    EXPECT_EQ(key, kFnvWTest);
    for (std::size_t i = 0; i < kUniform.size(); ++i)
        EXPECT_DOUBLE_EQ(dare_uniform(99, key, 1, i), kUniform[i]) << i;
    std::vector<float> out(kGoldenN);
    dare_chunk(kBase.data(), {kM1.data(), kM2.data()}, {1.0, 0.5}, 0.25, 99, key, 0, 1.0, kGoldenN,
               out.data());
    expect_close(out, kDare, "dare");  // the numpy golden uses lambda = 1
    // Evaluating the second half as its own chunk (first index 16) gives the same values as the whole run.
    std::vector<float> tail(kGoldenN - 16);
    dare_chunk(kBase.data() + 16, {kM1.data() + 16, kM2.data() + 16}, {1.0, 0.5}, 0.25, 99, key, 16, 1.0,
               kGoldenN - 16, tail.data());
    expect_close(tail, kDareTail, "dare tail chunk");
}

TEST(MergeMath, DareDropRateIsRespectedAndReproducible) {
    const std::uint64_t key = fnv1a64("x");
    std::size_t dropped = 0;
    const std::size_t n = 20000;
    for (std::size_t i = 0; i < n; ++i)
        if (dare_uniform(7, key, 0, i) < 0.3)
            ++dropped;
    EXPECT_NEAR(static_cast<double>(dropped) / n, 0.3, 0.02);
    EXPECT_EQ(dare_uniform(7, key, 0, 5), dare_uniform(7, key, 0, 5));
    EXPECT_NE(dare_uniform(7, key, 0, 5), dare_uniform(8, key, 0, 5));
    EXPECT_NE(dare_uniform(7, key, 0, 5), dare_uniform(7, key, 1, 5));
}

TEST(MergeMath, LoraRowsMatchNumpyAndAreRowBlockIndependent) {
    const std::size_t rows = 4, cols = 6, rank = 2;
    std::vector<float> out(rows * cols);
    lora_rows(kLoraBase.data(), kLoraB.data(), kLoraA.data(), rows, cols, rank, 0.75 * 4.0 / 2.0, out.data());
    expect_close(out, kLoraOut, "lora");
    std::vector<float> blocked(rows * cols);
    lora_rows(kLoraBase.data(), kLoraB.data(), kLoraA.data(), 3, cols, rank, 1.5, blocked.data());
    lora_rows(kLoraBase.data() + 3 * cols, kLoraB.data() + 3 * rank, kLoraA.data(), 1, cols, rank, 1.5,
              blocked.data() + 3 * cols);
    expect_close(blocked, kLoraOut, "lora blocked");
}

TEST(MergeDtypes, HalfAndBf16MatchNumpy) {
    for (std::size_t i = 0; i < kHalfIn.size(); ++i) {
        EXPECT_EQ(static_cast<unsigned>(float_to_half(kHalfIn[i])), kHalfBits[i]) << "half " << kHalfIn[i];
        EXPECT_EQ(static_cast<unsigned>(float_to_bf16(kHalfIn[i])), kBf16Bits[i]) << "bf16 " << kHalfIn[i];
    }
    // Round trips of exactly representable values.
    for (float v : {0.0f, 1.0f, -2.5f, 0.25f, -0.0f}) {
        EXPECT_EQ(half_to_float(float_to_half(v)), v);
        EXPECT_EQ(bf16_to_float(float_to_bf16(v)), v);
    }
    EXPECT_EQ(half_to_float(float_to_half(65504.0f)), 65504.0f);  // the largest finite half
    EXPECT_EQ(bf16_to_float(float_to_bf16(65504.0f)), 65536.0f);  // bf16 has 8 mantissa bits: rounds up
    // Every finite half survives half -> float -> half.
    for (unsigned h = 0; h < 65536; ++h) {
        const unsigned e = (h >> 10) & 0x1f;
        if (e == 31)
            continue;
        ASSERT_EQ(static_cast<unsigned>(float_to_half(half_to_float(static_cast<std::uint16_t>(h)))), h) << h;
    }
    EXPECT_TRUE(std::isnan(half_to_float(float_to_half(std::nanf("")))));
    EXPECT_TRUE(std::isinf(half_to_float(float_to_half(1e10f))));
    EXPECT_TRUE(std::isnan(bf16_to_float(float_to_bf16(std::nanf("")))));
}

TEST(MergeMath, ParseMethodAndNames) {
    EXPECT_EQ(parse_method("ties").value(), Method::Ties);
    EXPECT_EQ(parse_method("lora").value(), Method::Lora);
    EXPECT_FALSE(parse_method("average").ok());
    EXPECT_STREQ(to_string(Method::Slerp), "slerp");
}
