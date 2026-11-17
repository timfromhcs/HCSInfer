#include "hcs/storage/sha256.hpp"

#include <atomic>
#include <cstring>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#define HCS_X86_MSVC 1
#include <immintrin.h>
#include <intrin.h>
#endif

namespace hcs::storage {
namespace {

alignas(16) constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

void blocks_portable(std::uint32_t st[8], const std::uint8_t* p, std::size_t nblocks) {
    for (; nblocks; --nblocks, p += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        st[0] += a;
        st[1] += b;
        st[2] += c;
        st[3] += d;
        st[4] += e;
        st[5] += f;
        st[6] += g;
        st[7] += h;
    }
}

#ifdef HCS_X86_MSVC
bool cpu_has_sha() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7)
        return false;
    __cpuid(r, 1);
    const bool ssse3 = (r[2] & (1 << 9)) != 0;
    const bool sse41 = (r[2] & (1 << 19)) != 0;
    __cpuidex(r, 7, 0);
    const bool sha = (r[1] & (1 << 29)) != 0;
    return ssse3 && sse41 && sha;
}

void blocks_shani(std::uint32_t state[8], const std::uint8_t* data, std::size_t nblocks) {
    const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i TMP = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
    __m128i STATE1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
    TMP = _mm_shuffle_epi32(TMP, 0xB1);                // CDAB
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);          // EFGH
    __m128i STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);  // ABEF
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);       // CDGH

    for (; nblocks; --nblocks, data += 64) {
        const __m128i ABEF_SAVE = STATE0, CDGH_SAVE = STATE1;
        __m128i w[4];
        for (int j = 0; j < 16; ++j) {
            if (j < 4) {
                w[j] =
                    _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * j)), MASK);
            } else {
                const __m128i t = _mm_add_epi32(_mm_sha256msg1_epu32(w[j & 3], w[(j + 1) & 3]),
                                                _mm_alignr_epi8(w[(j + 3) & 3], w[(j + 2) & 3], 4));
                w[j & 3] = _mm_sha256msg2_epu32(t, w[(j + 3) & 3]);
            }
            __m128i msg =
                _mm_add_epi32(w[j & 3], _mm_load_si128(reinterpret_cast<const __m128i*>(&K[4 * j])));
            STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, msg);
            msg = _mm_shuffle_epi32(msg, 0x0E);
            STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, msg);
        }
        STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
        STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);
    }
    TMP = _mm_shuffle_epi32(STATE0, 0x1B);        // FEBA
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);     // DCHG
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);  // DCBA
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);     // HGFE
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), STATE0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), STATE1);
}
#endif

std::atomic<bool> g_force_portable{false};

bool hw_ok() noexcept {
#ifdef HCS_X86_MSVC
    static const bool has = cpu_has_sha();
    return has;
#else
    return false;
#endif
}

void process(std::uint32_t st[8], const std::uint8_t* p, std::size_t nblocks) noexcept {
#ifdef HCS_X86_MSVC
    if (!g_force_portable.load(std::memory_order_relaxed) && hw_ok()) {
        blocks_shani(st, p, nblocks);
        return;
    }
#endif
    blocks_portable(st, p, nblocks);
}

}  // namespace

bool sha256_hardware_available() noexcept {
    return hw_ok() && !g_force_portable.load();
}
void sha256_force_portable(bool force) noexcept {
    g_force_portable.store(force);
}
const char* sha256_implementation() noexcept {
    return sha256_hardware_available() ? "sha-ni" : "portable";
}

void Sha256::reset() noexcept {
    static constexpr std::uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(state_, init, sizeof(state_));
    total_ = 0;
    buffered_ = 0;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    total_ += size;
    if (buffered_) {
        const std::size_t take = std::min<std::size_t>(64 - buffered_, size);
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take;
        p += take;
        size -= take;
        if (buffered_ == 64) {
            process(state_, buffer_, 1);
            buffered_ = 0;
        }
    }
    if (size >= 64) {
        const std::size_t nb = size / 64;
        process(state_, p, nb);
        p += nb * 64;
        size -= nb * 64;
    }
    if (size) {
        std::memcpy(buffer_, p, size);
        buffered_ = size;
    }
}

Digest Sha256::finalize() noexcept {
    const std::uint64_t bits = total_ * 8;
    std::uint8_t pad[72] = {0x80};
    const std::size_t padlen = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
    std::uint8_t lenbytes[8];
    for (int i = 0; i < 8; ++i) lenbytes[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    const std::uint64_t saved_total = total_;
    update(pad, padlen);
    update(lenbytes, 8);
    (void)saved_total;
    Digest out{};
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(state_[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    reset();
    return out;
}

Digest sha256(std::span<const std::uint8_t> data) noexcept {
    Sha256 h;
    h.update(data);
    return h.finalize();
}

std::string to_hex(const Digest& d) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string s(64, '0');
    for (std::size_t i = 0; i < 32; ++i) {
        s[2 * i] = hex[d[i] >> 4];
        s[2 * i + 1] = hex[d[i] & 15];
    }
    return s;
}

bool digest_from_hex(const std::string& h, Digest& out) noexcept {
    if (h.size() != 64)
        return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < 32; ++i) {
        const int a = nib(h[2 * i]), b = nib(h[2 * i + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = static_cast<std::uint8_t>((a << 4) | b);
    }
    return true;
}

}  // namespace hcs::storage
