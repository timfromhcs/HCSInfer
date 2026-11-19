#include "hcs/kernels/cpu.hpp"

#include <cstring>

namespace hcs::kernels::cpu {

std::uint64_t q8_0_row_blocks(std::uint32_t k) {
    return k / kQ8_0BlockValues;
}
std::uint64_t q8_0_row_bytes(std::uint32_t k) {
    return q8_0_row_blocks(k) * kQ8_0BlockBytes;
}
std::uint64_t q8_0_bytes(std::uint32_t m, std::uint32_t k) {
    return m * q8_0_row_bytes(k);
}

float fp16_to_fp32(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t man = h & 0x3FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;  // +/- 0
        } else {
            // Subnormal half: normalise into a normal float.
            exp = 127 - 15 + 1;
            while ((man & 0x400u) == 0) {
                man <<= 1;
                --exp;
            }
            man &= 0x3FFu;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);  // Inf / NaN
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (man << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

std::uint16_t fp32_to_fp16(float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::int32_t exp = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const std::uint32_t man = bits & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10)
            return static_cast<std::uint16_t>(sign);  // underflows to zero
        // Subnormal.
        const std::uint32_t m = (man | 0x800000u) >> static_cast<std::uint32_t>(1 - exp);
        return static_cast<std::uint16_t>(sign | (m >> 13));
    }
    if (exp >= 31)
        return static_cast<std::uint16_t>(sign | 0x7C00u);  // overflow -> Inf
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10) | (man >> 13));
}

void gemv_q8_0(const float* x, const void* w_q8_0, float* y, std::uint32_t m, std::uint32_t k) {
    const auto* bytes = static_cast<const std::uint8_t*>(w_q8_0);
    const std::uint64_t blocks_per_row = q8_0_row_blocks(k);
    for (std::uint32_t row = 0; row < m; ++row) {
        const std::uint8_t* row_bytes = bytes + static_cast<std::uint64_t>(row) * q8_0_row_bytes(k);
        float acc = 0.0f;
        for (std::uint64_t b = 0; b < blocks_per_row; ++b) {
            const std::uint8_t* block = row_bytes + b * kQ8_0BlockBytes;
            std::uint16_t dh;
            std::memcpy(&dh, block, sizeof(dh));
            const float d = fp16_to_fp32(dh);
            for (std::uint32_t i = 0; i < kQ8_0BlockValues; ++i) {
                const std::int8_t q = static_cast<std::int8_t>(block[2 + i]);
                acc += d * static_cast<float>(q) * x[static_cast<std::uint64_t>(b) * kQ8_0BlockValues + i];
            }
        }
        y[row] = acc;
    }
}

}  // namespace hcs::kernels::cpu
