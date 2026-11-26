// Pure merge math: dtype conversions and the per-chunk kernels of every merge method.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "hcs/merge.hpp"

namespace hcs::merge {

const char* to_string(Method m) noexcept {
    switch (m) {
        case Method::Linear:
            return "linear";
        case Method::Slerp:
            return "slerp";
        case Method::Ties:
            return "ties";
        case Method::Dare:
            return "dare";
        case Method::Lora:
            return "lora";
    }
    return "unknown";
}

Result<Method> parse_method(std::string_view s) {
    if (s == "linear")
        return Method::Linear;
    if (s == "slerp")
        return Method::Slerp;
    if (s == "ties")
        return Method::Ties;
    if (s == "dare")
        return Method::Dare;
    if (s == "lora")
        return Method::Lora;
    return make_error(ErrorCode::InvalidArgument,
                      "unknown merge method '" + std::string(s) + "' (linear | slerp | ties | dare | lora)");
}

std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

double dare_uniform(std::uint64_t seed, std::uint64_t tensor_key, std::uint32_t model_index,
                    std::uint64_t index) noexcept {
    const std::uint64_t key =
        seed ^ tensor_key ^ (static_cast<std::uint64_t>(model_index) * 0xD6E8FEB86659FD93ull);
    std::uint64_t z = key + (index + 1) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

namespace {
std::uint32_t bits_of(float f) noexcept {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float float_of(std::uint32_t u) noexcept {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
}  // namespace

float half_to_float(std::uint16_t h) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1f;
    std::uint32_t mant = h & 0x3ff;
    if (exp == 0) {
        if (mant == 0)
            return float_of(sign);
        // subnormal: normalise
        int e = -1;
        do {
            ++e;
            mant <<= 1;
        } while ((mant & 0x400) == 0);
        mant &= 0x3ff;
        return float_of(sign | static_cast<std::uint32_t>(127 - 15 - e) << 23 | (mant << 13));
    }
    if (exp == 31)
        return float_of(sign | 0x7f800000u | (mant << 13));
    return float_of(sign | ((exp + 127 - 15) << 23) | (mant << 13));
}

std::uint16_t float_to_half(float f) noexcept {
    const std::uint32_t x = bits_of(f);
    const std::uint16_t sign = static_cast<std::uint16_t>((x >> 16) & 0x8000u);
    const std::int32_t exp = static_cast<std::int32_t>((x >> 23) & 0xff);
    std::uint32_t mant = x & 0x7fffffu;
    if (exp == 0xff)
        return static_cast<std::uint16_t>(sign | 0x7c00u | (mant ? 0x200u : 0u));
    const std::int32_t e = exp - 127 + 15;
    if (e >= 31)
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    if (e <= 0) {
        if (e < -10)
            return sign;
        mant |= 0x800000u;
        const int shift = 14 - e;
        std::uint32_t half = mant >> shift;
        const std::uint32_t rem = mant & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (half & 1u)))
            ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(e) << 10) | (mant >> 13);
    const std::uint32_t rem = mant & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u)))
        ++half;  // a carry into the exponent is the correct rounding (up to infinity included)
    return static_cast<std::uint16_t>(sign | half);
}

float bf16_to_float(std::uint16_t b) noexcept {
    return float_of(static_cast<std::uint32_t>(b) << 16);
}

std::uint16_t float_to_bf16(float f) noexcept {
    std::uint32_t x = bits_of(f);
    if ((x & 0x7fffffffu) > 0x7f800000u)
        return static_cast<std::uint16_t>((x >> 16) | 0x40u);  // keep NaN a NaN
    x += 0x7fffu + ((x >> 16) & 1u);
    return static_cast<std::uint16_t>(x >> 16);
}

void linear_chunk(const std::vector<const float*>& in, const std::vector<double>& w, std::size_t n,
                  float* out) {
    double wsum = 0.0;
    for (double v : w) wsum += v;
    for (std::size_t i = 0; i < n; ++i) {
        double acc = 0.0;
        for (std::size_t k = 0; k < in.size(); ++k) acc += w[k] * static_cast<double>(in[k][i]);
        out[i] = static_cast<float>(acc / wsum);
    }
}

void slerp_accumulate(const float* a, const float* b, std::size_t n, SlerpStats& s) {
    for (std::size_t i = 0; i < n; ++i) {
        const double x = a[i], y = b[i];
        s.dot += x * y;
        s.na += x * x;
        s.nb += y * y;
    }
}

SlerpCoeffs slerp_coeffs(const SlerpStats& s, double t) {
    const double denom = std::sqrt(s.na) * std::sqrt(s.nb);
    SlerpCoeffs c;
    c.ca = 1.0 - t;
    c.cb = t;
    if (!(denom > 0.0))
        return c;  // a zero vector has no direction: plain lerp
    const double d = s.dot / denom;
    if (std::fabs(d) > 0.9995)
        return c;  // (anti)parallel: slerp is numerically a lerp
    const double theta = std::acos(std::clamp(d, -1.0, 1.0));
    const double st = std::sin(theta);
    c.ca = std::sin((1.0 - t) * theta) / st;
    c.cb = std::sin(t * theta) / st;
    return c;
}

void slerp_apply(const float* a, const float* b, std::size_t n, const SlerpCoeffs& c, float* out) {
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<float>(c.ca * static_cast<double>(a[i]) + c.cb * static_cast<double>(b[i]));
}

float ties_select(std::vector<float>& v, double density) {
    const std::size_t n = v.size();
    if (n == 0)
        return 0.0f;
    const std::size_t k =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(density * static_cast<double>(n))));
    const std::size_t idx = n - std::min(k, n);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
    return v[idx];
}

float ties_threshold(const float* model, const float* base, std::size_t n, double density,
                     std::vector<float>& scratch) {
    scratch.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        scratch[i] =
            static_cast<float>(std::fabs(static_cast<double>(model[i]) - static_cast<double>(base[i])));
    return ties_select(scratch, density);
}

void ties_chunk(const float* base, const std::vector<const float*>& models, const std::vector<double>& w,
                const std::vector<float>& thr, double lambda, std::size_t n, float* out) {
    const std::size_t m = models.size();
    std::vector<double> weighted(m);
    for (std::size_t i = 0; i < n; ++i) {
        const double b = base[i];
        double sum = 0.0;
        for (std::size_t k = 0; k < m; ++k) {
            const double d = static_cast<double>(models[k][i]) - b;
            // The threshold was computed from float magnitudes; compare in the same domain.
            const bool keep = static_cast<float>(std::fabs(d)) >= thr[k];
            weighted[k] = keep ? w[k] * d : 0.0;
            sum += weighted[k];
        }
        const int elected = sum > 0.0 ? 1 : (sum < 0.0 ? -1 : 0);
        double num = 0.0, den = 0.0;
        if (elected != 0) {
            for (std::size_t k = 0; k < m; ++k) {
                const int s = weighted[k] > 0.0 ? 1 : (weighted[k] < 0.0 ? -1 : 0);
                if (s == elected) {
                    num += weighted[k];
                    den += w[k];
                }
            }
        }
        const double merged = den > 0.0 ? num / den : 0.0;
        out[i] = static_cast<float>(b + lambda * merged);
    }
}

void dare_chunk(const float* base, const std::vector<const float*>& models, const std::vector<double>& w,
                double p, std::uint64_t seed, std::uint64_t key, std::uint64_t first, double lambda,
                std::size_t n, float* out) {
    const double rescale = 1.0 / (1.0 - p);
    for (std::size_t i = 0; i < n; ++i) {
        const double b = base[i];
        double acc = 0.0;
        for (std::size_t k = 0; k < models.size(); ++k) {
            if (dare_uniform(seed, key, static_cast<std::uint32_t>(k), first + i) < p)
                continue;  // dropped
            acc += w[k] * (static_cast<double>(models[k][i]) - b) * rescale;
        }
        out[i] = static_cast<float>(b + lambda * acc);
    }
}

void lora_rows(const float* base, const float* b_rows, const float* a, std::size_t rows, std::size_t cols,
               std::size_t rank, double scale, float* out) {
    std::vector<double> tmp(cols);
    for (std::size_t r = 0; r < rows; ++r) {
        std::fill(tmp.begin(), tmp.end(), 0.0);
        for (std::size_t k = 0; k < rank; ++k) {
            const double bv = b_rows[r * rank + k];
            if (bv == 0.0)
                continue;
            const float* arow = a + k * cols;
            for (std::size_t c = 0; c < cols; ++c) tmp[c] += bv * static_cast<double>(arow[c]);
        }
        for (std::size_t c = 0; c < cols; ++c)
            out[r * cols + c] = static_cast<float>(static_cast<double>(base[r * cols + c]) + scale * tmp[c]);
    }
}

}  // namespace hcs::merge
