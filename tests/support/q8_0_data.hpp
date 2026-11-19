#pragma once
// Test helpers to build deterministic Q8_0 weight data (ggml block_q8_0 layout) for the kernel tests.
// Test-only: this is a naive per-block symmetric quantiser, not a model quantiser.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "hcs/kernels/cpu.hpp"

namespace hcs::test {

inline std::vector<float> random_vec(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (auto& x : v) x = dist(rng);
    return v;
}

// Quantise an m x k float matrix to raw Q8_0 bytes. k must be a multiple of 32.
inline std::vector<std::uint8_t> to_q8_0(const std::vector<float>& w, std::uint32_t m, std::uint32_t k) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kernels::cpu::q8_0_bytes(m, k)), 0);
    const std::uint32_t blocks = k / kernels::cpu::kQ8_0BlockValues;
    for (std::uint32_t row = 0; row < m; ++row) {
        for (std::uint32_t b = 0; b < blocks; ++b) {
            const float* src =
                w.data() + static_cast<std::size_t>(row) * k + static_cast<std::size_t>(b) * 32;
            float amax = 0.0f;
            for (std::uint32_t i = 0; i < 32; ++i) amax = std::max(amax, std::fabs(src[i]));
            const float d = amax > 0.0f ? amax / 127.0f : 0.0f;
            std::uint8_t* dst =
                out.data() + (static_cast<std::size_t>(row) * blocks + b) * kernels::cpu::kQ8_0BlockBytes;
            const std::uint16_t dh = kernels::cpu::fp32_to_fp16(d);
            std::memcpy(dst, &dh, sizeof(dh));
            for (std::uint32_t i = 0; i < 32; ++i) {
                int q = (d > 0.0f) ? static_cast<int>(std::lround(src[i] / d)) : 0;
                q = std::clamp(q, -127, 127);
                dst[2 + i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(q));
            }
        }
    }
    return out;
}

}  // namespace hcs::test
