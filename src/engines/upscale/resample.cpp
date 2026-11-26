// Lanczos-3 resampling (separable, float, normalised weights).
#include <algorithm>
#include <cmath>

#include "hcs/upscale.hpp"

namespace hcs::upscale {

namespace {

constexpr double kPi = 3.14159265358979323846;

double lanczos3(double x) {
    x = std::fabs(x);
    if (x < 1e-12)
        return 1.0;
    if (x >= 3.0)
        return 0.0;
    const double px = kPi * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}

struct Taps {
    std::int64_t first = 0;      // first source index (may be < 0 before clamping)
    std::vector<float> weights;  // one per source index starting at `first`
};

std::vector<Taps> make_taps(std::uint32_t in, std::uint32_t out) {
    std::vector<Taps> taps(out);
    const double scale = static_cast<double>(in) / static_cast<double>(out);
    const double fscale = std::max(1.0, scale);
    const double support = 3.0 * fscale;
    for (std::uint32_t o = 0; o < out; ++o) {
        const double center = (static_cast<double>(o) + 0.5) * scale - 0.5;
        const std::int64_t lo = static_cast<std::int64_t>(std::ceil(center - support));
        const std::int64_t hi = static_cast<std::int64_t>(std::floor(center + support));
        Taps& t = taps[o];
        t.first = lo;
        double sum = 0.0;
        std::vector<double> w;
        w.reserve(static_cast<std::size_t>(hi - lo + 1));
        for (std::int64_t i = lo; i <= hi; ++i) {
            const double v = lanczos3((static_cast<double>(i) - center) / fscale);
            w.push_back(v);
            sum += v;
        }
        if (sum == 0.0) {  // cannot happen for a symmetric support, but never divide by zero
            sum = 1.0;
            w.assign(w.size(), 0.0);
            w[w.size() / 2] = 1.0;
        }
        t.weights.reserve(w.size());
        for (double v : w) t.weights.push_back(static_cast<float>(v / sum));
    }
    return taps;
}

inline std::int64_t clampi(std::int64_t v, std::int64_t lo, std::int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

Rgb8 resample_lanczos(const float* rgb, std::uint32_t w, std::uint32_t h, std::uint32_t out_w,
                      std::uint32_t out_h) {
    Rgb8 out;
    out.width = out_w;
    out.height = out_h;
    out.pixels.assign(static_cast<std::size_t>(out_w) * out_h * 3, 0);
    if (w == 0 || h == 0 || out_w == 0 || out_h == 0 || !rgb)
        return out;

    const auto xt = make_taps(w, out_w);
    const auto yt = make_taps(h, out_h);

    // Horizontal pass: h rows x out_w columns (float).
    std::vector<float> mid(static_cast<std::size_t>(out_w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        const float* row = rgb + static_cast<std::size_t>(y) * w * 3;
        float* mrow = mid.data() + static_cast<std::size_t>(y) * out_w * 3;
        for (std::uint32_t x = 0; x < out_w; ++x) {
            const Taps& t = xt[x];
            float r = 0, g = 0, b = 0;
            for (std::size_t k = 0; k < t.weights.size(); ++k) {
                const std::int64_t sx =
                    clampi(t.first + static_cast<std::int64_t>(k), 0, static_cast<std::int64_t>(w) - 1);
                const float wt = t.weights[k];
                const float* p = row + static_cast<std::size_t>(sx) * 3;
                r += wt * p[0];
                g += wt * p[1];
                b += wt * p[2];
            }
            mrow[static_cast<std::size_t>(x) * 3 + 0] = r;
            mrow[static_cast<std::size_t>(x) * 3 + 1] = g;
            mrow[static_cast<std::size_t>(x) * 3 + 2] = b;
        }
    }
    // Vertical pass straight to 8 bit.
    std::vector<float> acc(static_cast<std::size_t>(out_w) * 3);
    for (std::uint32_t y = 0; y < out_h; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        const Taps& t = yt[y];
        for (std::size_t k = 0; k < t.weights.size(); ++k) {
            const std::int64_t sy =
                clampi(t.first + static_cast<std::int64_t>(k), 0, static_cast<std::int64_t>(h) - 1);
            const float wt = t.weights[k];
            const float* mrow = mid.data() + static_cast<std::size_t>(sy) * out_w * 3;
            for (std::size_t i = 0; i < acc.size(); ++i) acc[i] += wt * mrow[i];
        }
        std::uint8_t* orow = out.pixels.data() + static_cast<std::size_t>(y) * out_w * 3;
        for (std::size_t i = 0; i < acc.size(); ++i) {
            const float v = std::clamp(acc[i], 0.0f, 1.0f) * 255.0f + 0.5f;
            orow[i] = static_cast<std::uint8_t>(v);
        }
    }
    return out;
}

}  // namespace hcs::upscale
