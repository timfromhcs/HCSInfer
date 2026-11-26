#include "hcs/kernels/cpu_baseline.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

namespace hcs::kernels::cpu {

namespace {

// Runs fn(begin, end) over [0, items) on up to `threads` threads; each thread gets >= min_items items.
void parallel_for(std::size_t items, std::size_t min_items, unsigned threads,
                  const std::function<void(std::size_t, std::size_t)>& fn) {
    if (items == 0)
        return;
    unsigned hw = threads != 0 ? threads : std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 1;
    const std::size_t by_work = std::max<std::size_t>(1, items / std::max<std::size_t>(min_items, 1));
    const std::size_t nt = std::min<std::size_t>(hw, by_work);
    if (nt <= 1) {
        fn(0, items);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(nt - 1);
    const std::size_t chunk = (items + nt - 1) / nt;
    for (std::size_t t = 1; t < nt; ++t) {
        const std::size_t b = t * chunk, e = std::min(items, b + chunk);
        if (b < e)
            pool.emplace_back([&fn, b, e] { fn(b, e); });
    }
    fn(0, std::min(items, chunk));
    for (auto& th : pool) th.join();
}

}  // namespace

void baseline_gemm_f32(const float* a, const float* b, float* c, std::size_t m, std::size_t n, std::size_t k,
                       unsigned threads) {
    // Rows of C are independent; ~1 MFLOP of work per thread before splitting.
    const std::size_t flops_per_row = std::max<std::size_t>(2 * n * k, 1);
    parallel_for(m, std::max<std::size_t>(1, 1000000 / flops_per_row), threads,
                 [=](std::size_t r0, std::size_t r1) {
                     for (std::size_t r = r0; r < r1; ++r) {
                         float* cr = c + r * n;
                         std::fill(cr, cr + n, 0.0f);
                         for (std::size_t i = 0; i < k; ++i) {
                             const float av = a[r * k + i];
                             const float* br = b + i * n;
                             for (std::size_t j = 0; j < n; ++j) cr[j] += av * br[j];
                         }
                     }
                 });
}

void baseline_gemm_i8(const std::int8_t* a, const std::int8_t* b, std::int32_t* c, std::size_t m,
                      std::size_t n, std::size_t k, unsigned threads) {
    const std::size_t flops_per_row = std::max<std::size_t>(2 * n * k, 1);
    parallel_for(m, std::max<std::size_t>(1, 1000000 / flops_per_row), threads,
                 [=](std::size_t r0, std::size_t r1) {
                     for (std::size_t r = r0; r < r1; ++r) {
                         std::int32_t* cr = c + r * n;
                         std::fill(cr, cr + n, 0);
                         for (std::size_t i = 0; i < k; ++i) {
                             const std::int32_t av = a[r * k + i];
                             const std::int8_t* br = b + i * n;
                             for (std::size_t j = 0; j < n; ++j)
                                 cr[j] += av * static_cast<std::int32_t>(br[j]);
                         }
                     }
                 });
}

void baseline_softmax_rows(const float* x, float* y, std::size_t rows, std::size_t cols, unsigned threads) {
    if (cols == 0)
        return;
    parallel_for(rows, std::max<std::size_t>(1, 200000 / cols), threads, [=](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            const float* xr = x + r * cols;
            float* yr = y + r * cols;
            float mx = -INFINITY;
            for (std::size_t i = 0; i < cols; ++i) mx = std::max(mx, xr[i]);
            float sum = 0.0f;
            for (std::size_t i = 0; i < cols; ++i) {
                yr[i] = std::exp(xr[i] - mx);
                sum += yr[i];
            }
            if (sum == 0.0f || !std::isfinite(sum)) {
                std::fill(yr, yr + cols, 1.0f / static_cast<float>(cols));
                continue;
            }
            const float inv = 1.0f / sum;
            for (std::size_t i = 0; i < cols; ++i) yr[i] *= inv;
        }
    });
}

void baseline_layernorm_rows(const float* x, const float* gamma, const float* beta, float* y,
                             std::size_t rows, std::size_t cols, float eps, unsigned threads) {
    if (cols == 0)
        return;
    parallel_for(rows, std::max<std::size_t>(1, 200000 / cols), threads, [=](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            const float* xr = x + r * cols;
            float mean = 0.0f;
            for (std::size_t i = 0; i < cols; ++i) mean += xr[i];
            mean /= static_cast<float>(cols);
            float var = 0.0f;
            for (std::size_t i = 0; i < cols; ++i) {
                const float d = xr[i] - mean;
                var += d * d;
            }
            var /= static_cast<float>(cols);
            const float inv = 1.0f / std::sqrt(var + eps);
            for (std::size_t i = 0; i < cols; ++i)
                y[r * cols + i] = (xr[i] - mean) * inv * gamma[i] + beta[i];
        }
    });
}

void baseline_rmsnorm_rows(const float* x, const float* gamma, float* y, std::size_t rows, std::size_t cols,
                           float eps, unsigned threads) {
    if (cols == 0)
        return;
    parallel_for(rows, std::max<std::size_t>(1, 200000 / cols), threads, [=](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            const float* xr = x + r * cols;
            float ms = 0.0f;
            for (std::size_t i = 0; i < cols; ++i) ms += xr[i] * xr[i];
            ms /= static_cast<float>(cols);
            const float inv = 1.0f / std::sqrt(ms + eps);
            for (std::size_t i = 0; i < cols; ++i) y[r * cols + i] = xr[i] * inv * gamma[i];
        }
    });
}

void baseline_rope_rows(const float* x, float* y, std::size_t rows, std::size_t dim, std::uint32_t pos0,
                        float base, unsigned threads) {
    if (dim < 2 || dim % 2 != 0)
        return;
    const std::size_t half = dim / 2;
    std::vector<float> inv_freq(half);  // computed once per call, as production RoPE code does
    for (std::size_t i = 0; i < half; ++i)
        inv_freq[i] = std::pow(base, -2.0f * static_cast<float>(i) / static_cast<float>(dim));
    parallel_for(rows, std::max<std::size_t>(1, 20000 / half), threads, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            const float pos = static_cast<float>(pos0 + r);
            for (std::size_t i = 0; i < half; ++i) {
                const float theta = pos * inv_freq[i];
                const float s = std::sin(theta), c = std::cos(theta);
                const float a = x[r * dim + i], b = x[r * dim + i + half];
                y[r * dim + i] = a * c - b * s;
                y[r * dim + i + half] = a * s + b * c;
            }
        }
    });
}

}  // namespace hcs::kernels::cpu
