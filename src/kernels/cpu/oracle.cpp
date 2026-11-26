// CPU reference oracle (ported from hcsuda @94a3485 crates/hcsuda-core/src/reference.rs, dtype.rs; see
// docs/hcsuda-port.md). Written for clarity, not speed.
#include "hcs/kernels/oracle.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "hcs/kernels/cpu.hpp"

namespace hcs::kernels::cpu {

Tolerance Tolerance::for_len(std::size_t len) const {
    return Tolerance{rel, abs * std::sqrt(static_cast<double>(std::max<std::size_t>(len, 1)))};
}

bool Tolerance::accepts(float want, float got) const {
    if (std::isnan(want) || std::isnan(got))
        return std::isnan(want) && std::isnan(got);
    if (std::isinf(want) || std::isinf(got))
        return want == got;
    const double diff = std::fabs(static_cast<double>(want) - static_cast<double>(got));
    return diff <= abs + rel * std::fabs(static_cast<double>(want));
}

Tolerance tolerance_f32() {
    return Tolerance{1e-5, 1e-6};
}
Tolerance tolerance_f16() {
    return Tolerance{1e-2, 1e-3};
}
Tolerance tolerance_exact() {
    return Tolerance{0.0, 0.0};
}

Comparison compare(const float* want, const float* got, std::size_t n, const Tolerance& tol) {
    Comparison r;
    r.count = n;
    for (std::size_t i = 0; i < n; ++i) {
        const bool wn = std::isnan(want[i]), gn = std::isnan(got[i]);
        double diff = 0.0;
        if (wn || gn) {
            if (!(wn && gn))
                diff = INFINITY;
        } else if (std::isinf(want[i]) || std::isinf(got[i])) {
            if (want[i] != got[i])
                diff = INFINITY;
        } else {
            diff = std::fabs(static_cast<double>(want[i]) - static_cast<double>(got[i]));
        }
        if (diff > r.max_abs_error) {
            r.max_abs_error = diff;
            r.worst_index = i;
        }
        if (!tol.accepts(want[i], got[i]))
            ++r.mismatches;
    }
    r.ok = r.mismatches == 0;
    return r;
}

Comparison compare_exact(const std::int32_t* want, const std::int32_t* got, std::size_t n) {
    Comparison r;
    r.count = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (want[i] != got[i]) {
            const double diff = std::fabs(static_cast<double>(want[i]) - static_cast<double>(got[i]));
            if (diff > r.max_abs_error) {
                r.max_abs_error = diff;
                r.worst_index = i;
            }
            ++r.mismatches;
        }
    }
    r.ok = r.mismatches == 0;
    return r;
}

void gemm_f32(const float* a, const float* b, float* c, std::size_t m, std::size_t n, std::size_t k) {
    for (std::size_t r = 0; r < m; ++r) {
        for (std::size_t col = 0; col < n; ++col) {
            double acc = 0.0;
            for (std::size_t i = 0; i < k; ++i)
                acc += static_cast<double>(a[r * k + i]) * static_cast<double>(b[i * n + col]);
            c[r * n + col] = static_cast<float>(acc);
        }
    }
}

void gemm_f16(const std::uint16_t* a, const std::uint16_t* b, float* c, std::size_t m, std::size_t n,
              std::size_t k) {
    std::vector<float> af(m * k), bf(k * n);
    for (std::size_t i = 0; i < af.size(); ++i)
        af[i] = fp16_to_fp32(a[i]);
    for (std::size_t i = 0; i < bf.size(); ++i)
        bf[i] = fp16_to_fp32(b[i]);
    gemm_f32(af.data(), bf.data(), c, m, n, k);
}

void gemm_i8(const std::int8_t* a, const std::int8_t* b, std::int32_t* c, std::size_t m, std::size_t n,
             std::size_t k) {
    for (std::size_t r = 0; r < m; ++r) {
        for (std::size_t col = 0; col < n; ++col) {
            std::int64_t acc = 0;
            for (std::size_t i = 0; i < k; ++i)
                acc += static_cast<std::int64_t>(a[r * k + i]) * static_cast<std::int64_t>(b[i * n + col]);
            c[r * n + col] = static_cast<std::int32_t>(acc);
        }
    }
}

void softmax_rows(const float* x, float* y, std::size_t rows, std::size_t cols) {
    if (cols == 0)
        return;
    for (std::size_t r = 0; r < rows; ++r) {
        const float* xr = x + r * cols;
        float* yr = y + r * cols;
        float mx = -INFINITY;
        for (std::size_t i = 0; i < cols; ++i)
            mx = std::max(mx, xr[i]);
        double sum = 0.0;
        for (std::size_t i = 0; i < cols; ++i)
            sum += std::exp(static_cast<double>(xr[i]) - static_cast<double>(mx));
        if (sum == 0.0 || !std::isfinite(sum)) {
            for (std::size_t i = 0; i < cols; ++i)
                yr[i] = 1.0f / static_cast<float>(cols);
            continue;
        }
        for (std::size_t i = 0; i < cols; ++i)
            yr[i] = static_cast<float>(std::exp(static_cast<double>(xr[i]) - static_cast<double>(mx)) / sum);
    }
}

void layernorm_rows(const float* x, const float* gamma, const float* beta, float* y, std::size_t rows,
                    std::size_t cols, float eps) {
    if (cols == 0)
        return;
    for (std::size_t r = 0; r < rows; ++r) {
        const float* xr = x + r * cols;
        double mean = 0.0;
        for (std::size_t i = 0; i < cols; ++i)
            mean += xr[i];
        mean /= static_cast<double>(cols);
        double var = 0.0;
        for (std::size_t i = 0; i < cols; ++i) {
            const double d = xr[i] - mean;
            var += d * d;
        }
        var /= static_cast<double>(cols);
        const double inv = 1.0 / std::sqrt(var + static_cast<double>(eps));
        for (std::size_t i = 0; i < cols; ++i)
            y[r * cols + i] = static_cast<float>((xr[i] - mean) * inv * gamma[i] + beta[i]);
    }
}

void rmsnorm_rows(const float* x, const float* gamma, float* y, std::size_t rows, std::size_t cols,
                  float eps) {
    if (cols == 0)
        return;
    for (std::size_t r = 0; r < rows; ++r) {
        const float* xr = x + r * cols;
        double ms = 0.0;
        for (std::size_t i = 0; i < cols; ++i)
            ms += static_cast<double>(xr[i]) * xr[i];
        ms /= static_cast<double>(cols);
        const double inv = 1.0 / std::sqrt(ms + static_cast<double>(eps));
        for (std::size_t i = 0; i < cols; ++i)
            y[r * cols + i] = static_cast<float>(xr[i] * inv * gamma[i]);
    }
}

void rope_rows(const float* x, float* y, std::size_t rows, std::size_t dim, std::uint32_t pos0, float base) {
    if (dim < 2 || dim % 2 != 0)
        return;
    const std::size_t half = dim / 2;
    for (std::size_t r = 0; r < rows; ++r) {
        const double pos = static_cast<double>(pos0) + static_cast<double>(r);
        for (std::size_t i = 0; i < half; ++i) {
            const double theta =
                pos * std::pow(static_cast<double>(base), -2.0 * static_cast<double>(i) / static_cast<double>(dim));
            const double s = std::sin(theta), c = std::cos(theta);
            const double a = x[r * dim + i], b = x[r * dim + i + half];
            y[r * dim + i] = static_cast<float>(a * c - b * s);
            y[r * dim + i + half] = static_cast<float>(a * s + b * c);
        }
    }
}

}  // namespace hcs::kernels::cpu
