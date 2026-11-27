// Tiled upscale pipeline: planning (memory-driven tile size), feathered blending, model pass, Lanczos finish.
#include <algorithm>
#include <chrono>
#include <cmath>

#include "hcs/checked_math.hpp"
#include "hcs/upscale.hpp"

namespace hcs::upscale {

namespace {
using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
constexpr std::uint32_t kTileCandidates[] = {512, 384, 256, 192, 128, 96, 64};
constexpr std::uint32_t kDefaultOverlap = 16;
}  // namespace

std::vector<TileRect> plan_tiles(std::uint32_t w, std::uint32_t h, std::uint32_t tile,
                                 std::uint32_t overlap) {
    std::vector<TileRect> out;
    if (w == 0 || h == 0 || tile == 0)
        return out;
    if (overlap >= tile)
        overlap = tile - 1;
    const std::uint32_t stride = tile - overlap;
    auto starts = [&](std::uint32_t n) {
        std::vector<std::uint32_t> s;
        if (n <= tile) {
            s.push_back(0);
            return s;
        }
        std::uint32_t p = 0;
        while (true) {
            if (p + tile >= n) {
                s.push_back(n - tile);  // last tile aligned to the edge
                break;
            }
            s.push_back(p);
            p += stride;
        }
        return s;
    };
    const auto xs = starts(w);
    const auto ys = starts(h);
    const std::uint32_t tw = std::min(tile, w), th = std::min(tile, h);
    for (auto y : ys)
        for (auto x : xs) out.push_back(TileRect{x, y, tw, th});
    return out;
}

void target_size(std::uint32_t w, std::uint32_t h, std::uint32_t target, std::uint32_t& out_w,
                 std::uint32_t& out_h) {
    if (w == 0 || h == 0 || target == 0) {
        out_w = out_h = 0;
        return;
    }
    if (w >= h) {
        out_w = target;
        out_h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::llround(
                                               static_cast<double>(h) * target / static_cast<double>(w))));
    } else {
        out_h = target;
        out_w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::llround(
                                               static_cast<double>(w) * target / static_cast<double>(h))));
    }
}

std::uint64_t estimate_peak_bytes(std::uint32_t sw, std::uint32_t sh, std::uint32_t tile, const ModelInfo& m,
                                  std::uint32_t out_w, std::uint32_t out_h) {
    const std::uint64_t s = static_cast<std::uint64_t>(m.scale);
    const std::uint64_t tw = std::min(tile, sw), th = std::min(tile, sh);
    const std::uint64_t mw = sw * s, mh = sh * s;
    const std::uint64_t src = 3ull * sw * sh;
    // Stage 1: float RGB accumulator + weight plane at model resolution, one tile in/out, engine working set.
    const std::uint64_t stage1 = 16ull * mw * mh + 12ull * tw * th + 12ull * tw * th * s * s +
                                 m.working_bytes_per_input_pixel * tw * th;
    // Stage 2: accumulator (weights freed) + horizontal Lanczos pass + 8-bit result.
    const std::uint64_t stage2 = 12ull * mw * mh + 12ull * out_w * mh + 3ull * out_w * out_h;
    return src + std::max(stage1, stage2);
}

Result<Plan> plan_upscale(std::uint32_t w, std::uint32_t h, const ModelInfo& model, const Options& opt) {
    if (w == 0 || h == 0)
        return make_error(ErrorCode::InvalidArgument, "source image has no pixels");
    if (static_cast<std::uint64_t>(w) * h > kMaxSourcePixels)
        return make_error(ErrorCode::InvalidArgument, "source image exceeds the upscaler input limit of " +
                                                          std::to_string(kMaxSourcePixels) + " pixels");
    if (model.scale < 1 || model.scale > 8)
        return make_error(ErrorCode::Unsupported,
                          "model scale " + std::to_string(model.scale) + " is not supported");
    if (opt.target_long_edge < 64 || opt.target_long_edge > 16384)
        return make_error(ErrorCode::InvalidArgument, "target long edge must be 64..16384");
    Plan p;
    p.scale = model.scale;
    const std::uint32_t long_edge = std::max(w, h);
    if (long_edge >= opt.target_long_edge) {
        p.skip = true;
        p.skip_reason = "source long edge " + std::to_string(long_edge) + " is already >= the target " +
                        std::to_string(opt.target_long_edge) + "; never upscaling beyond the cap";
        p.out_w = w;
        p.out_h = h;
        return p;
    }
    p.model_w = w * static_cast<std::uint32_t>(model.scale);
    p.model_h = h * static_cast<std::uint32_t>(model.scale);
    target_size(w, h, opt.target_long_edge, p.out_w, p.out_h);
    const std::uint64_t budget = opt.memory_budget_bytes ? opt.memory_budget_bytes : kUnknownBudgetBytes;

    std::uint32_t tile = 0;
    if (model.fixed_tile > 0) {
        tile = model.fixed_tile;
        if (opt.tile != 0 && opt.tile != tile)
            return make_error(ErrorCode::InvalidArgument,
                              "this model only accepts " + std::to_string(tile) + " px tiles");
        const auto need = estimate_peak_bytes(w, h, tile, model, p.out_w, p.out_h);
        if (need > budget)
            return make_error(ErrorCode::OutOfMemory, "upscaling needs about " + std::to_string(need >> 20) +
                                                          " MiB but the budget is " +
                                                          std::to_string(budget >> 20) + " MiB");
        p.peak_bytes = need;
    } else if (opt.tile != 0) {
        if (opt.tile < 32 || opt.tile > 2048)
            return make_error(ErrorCode::InvalidArgument, "tile must be 32..2048");
        tile = opt.tile;
        p.peak_bytes = estimate_peak_bytes(w, h, tile, model, p.out_w, p.out_h);
        if (p.peak_bytes > budget)
            return make_error(ErrorCode::OutOfMemory,
                              "upscaling with tile " + std::to_string(tile) + " needs about " +
                                  std::to_string(p.peak_bytes >> 20) + " MiB but the budget is " +
                                  std::to_string(budget >> 20) + " MiB");
    } else {
        std::uint64_t smallest_need = 0;
        for (auto c : kTileCandidates) {
            const auto need = estimate_peak_bytes(w, h, c, model, p.out_w, p.out_h);
            smallest_need = need;
            if (need <= budget) {
                tile = c;
                p.peak_bytes = need;
                break;
            }
        }
        if (tile == 0)
            return make_error(ErrorCode::OutOfMemory,
                              "even the smallest tile needs about " + std::to_string(smallest_need >> 20) +
                                  " MiB but the budget is " + std::to_string(budget >> 20) +
                                  " MiB: free memory or upscale a smaller image");
    }
    std::uint32_t overlap = opt.overlap ? opt.overlap : kDefaultOverlap;
    overlap = std::min(overlap, tile / 4);
    p.tile = tile;
    p.overlap = overlap;
    p.tiles = plan_tiles(w, h, tile, overlap);
    return p;
}

float feather_weight(std::uint32_t i, std::uint32_t ramp) noexcept {
    if (ramp == 0 || i >= ramp)
        return 1.0f;
    return static_cast<float>(i + 1) / static_cast<float>(ramp + 1);
}

Result<Rgb8> upscale_image(IUpscaleEngine& engine, const Rgb8& orig, const Options& opt,
                           const CancellationToken& token, const ProgressFn& progress, Stats* stats) {
    Stats local;
    Stats& st = stats ? *stats : local;
    st = Stats{};
    st.src_w = orig.width;
    st.src_h = orig.height;
    if (orig.width == 0 || orig.height == 0 ||
        orig.pixels.size() != static_cast<std::size_t>(orig.width) * orig.height * 3)
        return make_error(ErrorCode::InvalidArgument,
                          "source image is empty or its buffer size is inconsistent");
    if (opt.pre_resize_long_edge != 0 &&
        (opt.pre_resize_long_edge < 16 || opt.pre_resize_long_edge > opt.target_long_edge))
        return make_error(ErrorCode::InvalidArgument, "pre-resize long edge must be 0 or 16..target");
    if (auto r = engine.ready(); !r)
        return r.error();
    // Optional pre-resize (Lanczos) so the model pass lands on the target without a final resample, e.g.
    // 512 -> 960 -> x4 = 3840. Never applied to a source that is already at/above the target (skipped below).
    Rgb8 pre;
    const Rgb8* use = &orig;
    if (const std::uint32_t le = std::max(orig.width, orig.height);
        opt.pre_resize_long_edge != 0 && le < opt.target_long_edge && le != opt.pre_resize_long_edge) {
        const auto t_pre = Clock::now();
        std::uint32_t pw = 0, ph = 0;
        target_size(orig.width, orig.height, opt.pre_resize_long_edge, pw, ph);
        std::vector<float> f(orig.pixels.size());
        for (std::size_t i = 0; i < f.size(); ++i) f[i] = static_cast<float>(orig.pixels[i]) / 255.0f;
        pre = resample_lanczos(f.data(), orig.width, orig.height, pw, ph);
        st.pre_resize_ms = ms_since(t_pre);
        use = &pre;
    }
    const Rgb8& src = *use;
    const ModelInfo mi = engine.info();
    st.provider = mi.provider;
    st.model = mi.name;
    auto planr = plan_upscale(src.width, src.height, mi, opt);
    if (!planr)
        return planr.error();
    const Plan& plan = planr.value();
    st.out_w = plan.out_w;
    st.out_h = plan.out_h;
    if (plan.skip) {
        st.skipped = true;
        st.skip_reason = plan.skip_reason;
        return orig;
    }
    st.tiles = static_cast<std::uint32_t>(plan.tiles.size());
    st.tile = plan.tile;
    st.overlap = plan.overlap;
    st.peak_bytes_estimate = plan.peak_bytes;

    const std::uint32_t s = static_cast<std::uint32_t>(plan.scale);
    const std::size_t mw = plan.model_w, mh = plan.model_h;
    std::vector<float> acc(mw * mh * 3, 0.0f);
    std::vector<float> wsum(mw * mh, 0.0f);

    const auto t_model = Clock::now();
    std::vector<float> in_buf, out_buf;
    std::vector<double> tile_ms;
    tile_ms.reserve(plan.tiles.size());
    double blend_ms = 0.0;
    const std::uint32_t overlap_px = plan.overlap * s;
    for (std::size_t ti = 0; ti < plan.tiles.size(); ++ti) {
        if (token.cancelled())
            return make_error(ErrorCode::Cancelled, "upscaling was cancelled");
        const TileRect& r = plan.tiles[ti];
        const std::uint32_t iw = mi.fixed_tile ? mi.fixed_tile : r.w;
        const std::uint32_t ih = mi.fixed_tile ? mi.fixed_tile : r.h;
        in_buf.assign(static_cast<std::size_t>(iw) * ih * 3, 0.0f);
        for (std::uint32_t y = 0; y < ih; ++y) {
            const std::uint32_t sy = std::min(r.y + y, src.height - 1);  // clamp: pads fixed-size tiles
            for (std::uint32_t x = 0; x < iw; ++x) {
                const std::uint32_t sx = std::min(r.x + x, src.width - 1);
                const std::uint8_t* p =
                    src.pixels.data() + (static_cast<std::size_t>(sy) * src.width + sx) * 3;
                float* d = in_buf.data() + (static_cast<std::size_t>(y) * iw + x) * 3;
                d[0] = static_cast<float>(p[0]) / 255.0f;
                d[1] = static_cast<float>(p[1]) / 255.0f;
                d[2] = static_cast<float>(p[2]) / 255.0f;
            }
        }
        out_buf.assign(static_cast<std::size_t>(iw) * s * ih * s * 3, 0.0f);
        const auto t_tile = Clock::now();
        if (auto rr = engine.run_tile(in_buf.data(), iw, ih, out_buf.data(), token); !rr)
            return rr.error();
        tile_ms.push_back(ms_since(t_tile));
        if (token.cancelled())
            return make_error(ErrorCode::Cancelled, "upscaling was cancelled");
        const auto t_blend = Clock::now();
        const std::size_t ostride = static_cast<std::size_t>(iw) * s;
        const bool left = r.x > 0, top = r.y > 0, right = r.x + r.w < src.width,
                   bottom = r.y + r.h < src.height;
        const std::uint32_t vw = r.w * s, vh = r.h * s;
        for (std::uint32_t y = 0; y < vh; ++y) {
            float wy = 1.0f;
            if (top)
                wy = std::min(wy, feather_weight(y, overlap_px));
            if (bottom)
                wy = std::min(wy, feather_weight(vh - 1 - y, overlap_px));
            const std::size_t gy = static_cast<std::size_t>(r.y) * s + y;
            for (std::uint32_t x = 0; x < vw; ++x) {
                float wx = 1.0f;
                if (left)
                    wx = std::min(wx, feather_weight(x, overlap_px));
                if (right)
                    wx = std::min(wx, feather_weight(vw - 1 - x, overlap_px));
                const float wt = wx * wy;
                const float* o = out_buf.data() + (static_cast<std::size_t>(y) * ostride + x) * 3;
                if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2]))
                    return make_error(
                        ErrorCode::BackendError,
                        "the upscale model produced non-finite values (NaN/Inf); an fp16 overflow on "
                        "the selected execution provider is the usual cause");
                const std::size_t gx = static_cast<std::size_t>(r.x) * s + x;
                const std::size_t gi = gy * mw + gx;
                acc[gi * 3 + 0] += wt * o[0];
                acc[gi * 3 + 1] += wt * o[1];
                acc[gi * 3 + 2] += wt * o[2];
                wsum[gi] += wt;
            }
        }
        blend_ms += ms_since(t_blend);
        if (progress)
            progress(0.9 * static_cast<double>(ti + 1) / static_cast<double>(plan.tiles.size()));
    }
    st.model_ms = ms_since(t_model);
    st.blend_ms = blend_ms;
    if (!tile_ms.empty()) {
        std::vector<double> sorted = tile_ms;
        std::sort(sorted.begin(), sorted.end());
        st.tile_ms_median = sorted[sorted.size() / 2];
        st.tile_ms_max = sorted.back();
    }
    for (std::size_t i = 0; i < mw * mh; ++i) {
        const float inv = wsum[i] > 0.0f ? 1.0f / wsum[i] : 0.0f;
        acc[i * 3 + 0] *= inv;
        acc[i * 3 + 1] *= inv;
        acc[i * 3 + 2] *= inv;
    }
    std::vector<float>().swap(wsum);
    if (token.cancelled())
        return make_error(ErrorCode::Cancelled, "upscaling was cancelled");
    const auto t_rs = Clock::now();
    Rgb8 out = resample_lanczos(acc.data(), static_cast<std::uint32_t>(mw), static_cast<std::uint32_t>(mh),
                                plan.out_w, plan.out_h);
    st.resample_ms = ms_since(t_rs);
    if (progress)
        progress(1.0);
    return out;
}

}  // namespace hcs::upscale
