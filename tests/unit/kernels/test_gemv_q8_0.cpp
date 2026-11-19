// CPU reference kernels: fp16 conversion, Q8_0 sizing, and the Q8_0 GEMV used to validate the Vulkan kernel.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "hcs/kernels/cpu.hpp"
#include "q8_0_data.hpp"

using namespace hcs;
using namespace hcs::kernels;

TEST(KernelCpuFp16, RoundTripsRepresentableValuesExactly) {
    // Dyadic rationals within the fp16 range round-trip exactly (0.1f and pi are NOT representable in fp16).
    for (float v : {0.0f, 1.0f, -1.0f, 0.5f, -0.25f, 2.0f, 1024.0f, -8.0f, 0.125f, 1.5f, 2048.0f, 0.0625f,
                    -0.0078125f}) {
        const std::uint16_t h = cpu::fp32_to_fp16(v);
        const float back = cpu::fp16_to_fp32(h);
        EXPECT_EQ(back, v) << "value " << v;
    }
}

TEST(KernelCpuFp16, HandlesSubnormalZeroInfAndKnownBitPatterns) {
    EXPECT_EQ(cpu::fp16_to_fp32(0x0000), 0.0f);
    EXPECT_EQ(std::signbit(cpu::fp16_to_fp32(0x8000)), true);  // -0
    EXPECT_EQ(cpu::fp16_to_fp32(0x3C00), 1.0f);
    EXPECT_EQ(cpu::fp16_to_fp32(0xBC00), -1.0f);
    EXPECT_EQ(cpu::fp16_to_fp32(0x7C00), std::numeric_limits<float>::infinity());
    // Smallest positive subnormal half is 2^-24.
    EXPECT_FLOAT_EQ(cpu::fp16_to_fp32(0x0001), std::ldexp(1.0f, -24));
}

TEST(KernelCpuSizing, RowAndTotalBytes) {
    EXPECT_EQ(cpu::q8_0_row_blocks(32), 1u);
    EXPECT_EQ(cpu::q8_0_row_blocks(4096), 128u);
    EXPECT_EQ(cpu::q8_0_row_bytes(32), 34u);
    EXPECT_EQ(cpu::q8_0_row_bytes(4096), 128u * 34u);
    EXPECT_EQ(cpu::q8_0_bytes(4, 64), 4u * 2u * 34u);
}

TEST(KernelCpuGemv, MatchesADirectDequantisedDotProduct) {
    const std::uint32_t m = 5, k = 96;  // 3 blocks per row
    const auto wf = test::random_vec(static_cast<std::size_t>(m) * k, 7);
    const auto w = test::to_q8_0(wf, m, k);
    const auto x = test::random_vec(k, 11);
    std::vector<float> y(m, 0.0f);
    cpu::gemv_q8_0(x.data(), w.data(), y.data(), m, k);

    // Independent expectation: dequantise with the same fp16 scales and take a plain dot product.
    const std::uint32_t blocks = k / 32;
    for (std::uint32_t row = 0; row < m; ++row) {
        double expect = 0.0;
        for (std::uint32_t b = 0; b < blocks; ++b) {
            const std::uint8_t* blk =
                w.data() + (static_cast<std::size_t>(row) * blocks + b) * cpu::kQ8_0BlockBytes;
            std::uint16_t dh;
            std::memcpy(&dh, blk, sizeof(dh));
            const double d = cpu::fp16_to_fp32(dh);
            for (std::uint32_t i = 0; i < 32; ++i) {
                const int q = static_cast<std::int8_t>(blk[2 + i]);
                expect += d * q * static_cast<double>(x[b * 32 + i]);
            }
        }
        EXPECT_NEAR(y[row], static_cast<float>(expect),
                    1e-3f * (1.0f + std::fabs(static_cast<float>(expect))));
    }
}

TEST(KernelCpuGemv, SingleRowSingleBlockAndZeroWeights) {
    const std::uint32_t m = 1, k = 32;
    const auto x = test::random_vec(k, 3);
    std::vector<std::uint8_t> zero(kernels::cpu::kQ8_0BlockBytes, 0);  // d = 0
    float y = -1.0f;
    cpu::gemv_q8_0(x.data(), zero.data(), &y, m, k);
    EXPECT_EQ(y, 0.0f);
}

TEST(KernelCpuGemv, NonPowerOfTwoRowsAndColumns) {
    const std::uint32_t m = 7, k = 160;  // 5 blocks
    const auto wf = test::random_vec(static_cast<std::size_t>(m) * k, 21);
    const auto w = test::to_q8_0(wf, m, k);
    const auto x = test::random_vec(k, 22);
    std::vector<float> y(m, 0.0f), y2(m, 0.0f);
    cpu::gemv_q8_0(x.data(), w.data(), y.data(), m, k);
    cpu::gemv_q8_0(x.data(), w.data(), y2.data(), m, k);
    EXPECT_EQ(y, y2) << "the reference must be deterministic";
    for (float v : y) EXPECT_TRUE(std::isfinite(v));
}
