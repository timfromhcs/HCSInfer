// In-process upstream GGML Vulkan operator baseline (see hcs/kernels/ggml_vk_baseline.hpp).
// This is the only HCS file besides the engine adapters that includes ggml headers; they never leave this TU.
#include "hcs/kernels/ggml_vk_baseline.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>

#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-vulkan.h"
#include "ggml.h"

namespace hcs::kernels::ggml_baseline {

namespace {

using Clock = std::chrono::steady_clock;

struct Graph {
    ggml_context* ctx = nullptr;
    ggml_backend_buffer_t buf = nullptr;
    ggml_cgraph* gf = nullptr;
    Graph() = default;
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    ~Graph() {
        if (buf)
            ggml_backend_buffer_free(buf);
        if (ctx)
            ggml_free(ctx);
    }
};

double median_of(std::vector<double> v) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}
double percentile_of(std::vector<double> v, double p) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = static_cast<std::size_t>(std::ceil(pos));
    return v[lo] + (v[hi] - v[lo]) * (pos - static_cast<double>(lo));
}

void quiet_log(ggml_log_level level, const char* text, void*) {
    if (level >= GGML_LOG_LEVEL_WARN && text != nullptr)
        std::fputs(text, stderr);
}

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::vector<float> rand_f32(std::size_t n, std::uint32_t seed, float lo, float hi) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> d(lo, hi);
    std::vector<float> v(n);
    for (auto& x : v) x = d(g);
    return v;
}

// Round to the nearest f16 and back, so a reference that sees the same values as an F16 tensor is exact.
void round_through_f16(std::vector<float>& v) {
    for (auto& x : v) x = ggml_fp16_to_fp32(ggml_fp32_to_fp16(x));
}

}  // namespace

std::string_view to_string(WeightType t) noexcept {
    switch (t) {
        case WeightType::F32:
            return "f32";
        case WeightType::F16:
            return "f16";
        case WeightType::Q8_0:
            return "q8_0";
        case WeightType::Q4_K:
            return "q4_k";
    }
    return "unknown";
}

struct Session::Impl {
    ggml_backend_t backend = nullptr;
    DeviceDescription dev;

    ~Impl() {
        if (backend)
            ggml_backend_free(backend);
    }

    // Allocates every tensor of `g` on the backend, after checking that the backend claims each node.
    Result<void> finalize(Graph& g) {
        const int n = ggml_graph_n_nodes(g.gf);
        for (int i = 0; i < n; ++i) {
            ggml_tensor* node = ggml_graph_node(g.gf, i);
            if (!ggml_backend_supports_op(backend, node))
                return make_error(
                    ErrorCode::Unsupported,
                    std::string("ggml Vulkan backend does not support op ") + ggml_op_name(node->op));
        }
        g.buf = ggml_backend_alloc_ctx_tensors(g.ctx, backend);
        if (!g.buf)
            return make_error(ErrorCode::OutOfMemory, "ggml_backend_alloc_ctx_tensors failed");
        return {};
    }

    Result<void> make_graph(Graph& g, std::size_t max_tensors) {
        ggml_init_params p{};
        p.mem_size = ggml_tensor_overhead() * max_tensors + ggml_graph_overhead();
        p.mem_buffer = nullptr;
        p.no_alloc = true;
        g.ctx = ggml_init(p);
        if (!g.ctx)
            return make_error(ErrorCode::Internal, "ggml_init failed");
        return {};
    }

    // Times `g` (already allocated and fed). `check` receives the output as f32 and returns the scale-free
    // error.
    Result<Measurement> time_graph(Graph& g, ggml_tensor* out, std::uint32_t samples, double flops,
                                   double tolerance,
                                   const std::function<double(const std::vector<float>&)>& check) {
        // Correctness first: one run, spot-checked.
        if (ggml_backend_graph_compute(backend, g.gf) != GGML_STATUS_SUCCESS)
            return make_error(ErrorCode::BackendError, "ggml_backend_graph_compute failed");
        std::vector<float> got(ggml_nelements(out));
        ggml_backend_tensor_get(out, got.data(), 0, got.size() * sizeof(float));
        Measurement m;
        m.flops = flops;
        m.max_err_over_l1 = check(got);
        m.check_ok = std::isfinite(m.max_err_over_l1) && m.max_err_over_l1 <= tolerance;
        if (!m.check_ok)
            return make_error(ErrorCode::BackendError,
                              "upstream operator result failed the spot check (error " +
                                  std::to_string(m.max_err_over_l1) + " > " + std::to_string(tolerance) +
                                  ")");
        // Warm-up (clock ramp, pipeline creation, prealloc buffers).
        {
            const auto t0 = Clock::now();
            int n = 0;
            while ((n < 10 || std::chrono::duration<double, std::milli>(Clock::now() - t0).count() < 250.0) &&
                   n < 400) {
                if (ggml_backend_graph_compute(backend, g.gf) != GGML_STATUS_SUCCESS)
                    return make_error(ErrorCode::BackendError, "graph compute failed during warm-up");
                ++n;
            }
        }
        for (std::uint32_t i = 0; i < samples; ++i) {
            const auto t0 = Clock::now();
            if (ggml_backend_graph_compute(backend, g.gf) != GGML_STATUS_SUCCESS)
                return make_error(ErrorCode::BackendError, "graph compute failed while timing");
            m.raw_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
        }
        m.samples = samples;
        m.median_ms = median_of(m.raw_ms);
        m.p10_ms = percentile_of(m.raw_ms, 0.1);
        m.p90_ms = percentile_of(m.raw_ms, 0.9);
        m.spread = m.median_ms > 0.0 ? (m.p90_ms - m.p10_ms) / m.median_ms : 0.0;
        return m;
    }
};

Session::Session(std::unique_ptr<Impl> i) : impl_(std::move(i)) {}
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() = default;

const DeviceDescription& Session::device() const {
    return impl_->dev;
}

Result<Session> Session::create(std::string_view substr) {
    ggml_log_set(quiet_log, nullptr);
    const int count = ggml_backend_vk_get_device_count();
    if (count <= 0)
        return make_error(ErrorCode::Unsupported, "the ggml Vulkan backend reports no device");
    int pick = -1;
    const std::string want = lower(substr);
    for (int i = 0; i < count; ++i) {
        char d[256] = {};
        ggml_backend_vk_get_device_description(i, d, sizeof(d));
        if (want.empty() || lower(d).find(want) != std::string::npos) {
            pick = i;
            break;
        }
    }
    if (pick < 0)
        return make_error(ErrorCode::NotFound, "no ggml Vulkan device matches '" + std::string(substr) + "'");
    auto impl = std::make_unique<Impl>();
    impl->backend = ggml_backend_vk_init(static_cast<std::size_t>(pick));
    if (!impl->backend)
        return make_error(ErrorCode::BackendError, "ggml_backend_vk_init failed");
    char d[256] = {};
    ggml_backend_vk_get_device_description(pick, d, sizeof(d));
    impl->dev.ggml_index = pick;
    impl->dev.description = d;
    impl->dev.backend = ggml_backend_name(impl->backend);
    std::size_t f = 0, t = 0;
    ggml_backend_vk_get_device_memory(pick, &f, &t);
    impl->dev.memory_free = f;
    impl->dev.memory_total = t;
    return Session(std::move(impl));
}

Result<Measurement> Session::mul_mat(WeightType w, std::uint32_t m, std::uint32_t n, std::uint32_t k,
                                     std::uint32_t samples) {
    if (m == 0 || n == 0 || k == 0)
        return make_error(ErrorCode::InvalidArgument, "mul_mat needs m, n, k > 0");
    ggml_type wt = GGML_TYPE_F32;
    switch (w) {
        case WeightType::F32:
            wt = GGML_TYPE_F32;
            break;
        case WeightType::F16:
            wt = GGML_TYPE_F16;
            break;
        case WeightType::Q8_0:
            wt = GGML_TYPE_Q8_0;
            break;
        case WeightType::Q4_K:
            wt = GGML_TYPE_Q4_K;
            break;
    }
    const auto* traits = ggml_get_type_traits(wt);
    if (k % static_cast<std::uint32_t>(traits->blck_size) != 0)
        return make_error(ErrorCode::InvalidArgument,
                          "k must be a multiple of the block size " + std::to_string(traits->blck_size));

    const std::uint32_t seed = 0xB45E0000u ^ (m * 31u + n * 7u + k);
    std::vector<float> x = rand_f32(std::size_t{m} * k, seed, -1.0f, 1.0f);
    std::vector<float> wf = rand_f32(std::size_t{n} * k, seed + 1, -1.0f, 1.0f);
    if (w == WeightType::F16) {
        round_through_f16(x);  // so F32-activation and F16-activation variants see identical values
        round_through_f16(wf);
    }
    // Weight bytes (+ the dequantised values the reference must use).
    std::vector<unsigned char> wbytes;
    std::vector<float> wref;  // only for quantised weights
    const std::size_t row_bytes = ggml_row_size(wt, k);
    if (w == WeightType::F32) {
        wbytes.resize(wf.size() * sizeof(float));
        std::memcpy(wbytes.data(), wf.data(), wbytes.size());
    } else if (w == WeightType::F16) {
        std::vector<ggml_fp16_t> h(wf.size());
        for (std::size_t i = 0; i < wf.size(); ++i) h[i] = ggml_fp32_to_fp16(wf[i]);
        wbytes.resize(h.size() * sizeof(ggml_fp16_t));
        std::memcpy(wbytes.data(), h.data(), wbytes.size());
    } else {
        wbytes.resize(row_bytes * n);
        ggml_quantize_chunk(wt, wf.data(), wbytes.data(), 0, n, k, nullptr);
        wref.resize(wf.size());
        for (std::uint32_t j = 0; j < n; ++j)
            traits->to_float(wbytes.data() + row_bytes * j, wref.data() + std::size_t{j} * k, k);
    }
    const std::vector<float>& wuse = w == WeightType::Q8_0 || w == WeightType::Q4_K ? wref : wf;

    // 16 deterministic (row, col) positions incl. both corners.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pos;
    {
        std::mt19937 g(seed ^ 0x5A5Au);
        pos.push_back({0, 0});
        pos.push_back({m - 1, n - 1});
        while (pos.size() < 16) pos.push_back({g() % m, g() % n});
    }
    const auto check = [&](const std::vector<float>& got) {
        double worst = 0.0;
        for (const auto& [i, j] : pos) {
            double ref = 0.0, l1 = 0.0;
            for (std::uint32_t t = 0; t < k; ++t) {
                const double term = static_cast<double>(x[std::size_t{i} * k + t]) *
                                    static_cast<double>(wuse[std::size_t{j} * k + t]);
                ref += term;
                l1 += std::fabs(term);
            }
            worst = std::max(
                worst, std::fabs(static_cast<double>(got[std::size_t{i} * n + j]) - ref) / (l1 + 1e-12));
        }
        return worst;
    };
    const double tol = w == WeightType::F32 ? 1e-4 : w == WeightType::F16 ? 1e-2 : 5e-2;
    const double flops = 2.0 * m * double(n) * double(k);

    struct Variant {
        ggml_type act;
        const char* name;
    };
    std::vector<Variant> variants = {{GGML_TYPE_F32, "mul_mat %s weights x f32 activations"}};
    if (w == WeightType::F16)
        variants.push_back({GGML_TYPE_F16, "mul_mat %s weights x f16 activations"});

    std::optional<Measurement> best;
    std::string notes;
    std::string last_err;
    for (const auto& v : variants) {
        Graph g;
        if (auto r = impl_->make_graph(g, 8); !r)
            return r.error();
        ggml_tensor* a = ggml_new_tensor_2d(g.ctx, wt, k, n);
        ggml_tensor* b = ggml_new_tensor_2d(g.ctx, v.act, k, m);
        ggml_tensor* out = ggml_mul_mat(g.ctx, a, b);
        g.gf = ggml_new_graph(g.ctx);
        ggml_build_forward_expand(g.gf, out);
        if (auto r = impl_->finalize(g); !r) {
            last_err = r.error().message;
            notes += std::string(" [") + ggml_type_name(v.act) + " activations: " + last_err + "]";
            continue;
        }
        ggml_backend_tensor_set(a, wbytes.data(), 0, wbytes.size());
        if (v.act == GGML_TYPE_F32) {
            ggml_backend_tensor_set(b, x.data(), 0, x.size() * sizeof(float));
        } else {
            std::vector<ggml_fp16_t> h(x.size());
            for (std::size_t i = 0; i < x.size(); ++i) h[i] = ggml_fp32_to_fp16(x[i]);
            ggml_backend_tensor_set(b, h.data(), 0, h.size() * sizeof(ggml_fp16_t));
        }
        auto r = impl_->time_graph(g, out, samples, flops, tol, check);
        if (!r) {
            last_err = r.error().message;
            notes += std::string(" [") + ggml_type_name(v.act) + " activations: " + last_err + "]";
            continue;
        }
        char name[96];
        std::snprintf(name, sizeof(name), v.name, ggml_type_name(wt));
        r.value().variant = name;
        notes +=
            std::string(" ") + ggml_type_name(v.act) + "-act " + std::to_string(r.value().median_ms) + " ms;";
        if (!best || r.value().median_ms < best->median_ms)
            best = std::move(r.value());
    }
    if (!best)
        return make_error(ErrorCode::Unsupported, "no upstream mul_mat variant ran:" + notes);
    best->note = variants.size() > 1 ? "fastest of:" + notes : std::string();
    return std::move(*best);
}

Result<Measurement> Session::softmax(std::uint32_t rows, std::uint32_t cols, std::uint32_t samples) {
    if (rows == 0 || cols == 0)
        return make_error(ErrorCode::InvalidArgument, "softmax needs rows, cols > 0");
    const std::vector<float> x =
        rand_f32(std::size_t{rows} * cols, 0xB45E1001u ^ (rows * 31u + cols), -8.0f, 8.0f);
    Graph g;
    if (auto r = impl_->make_graph(g, 4); !r)
        return r.error();
    ggml_tensor* in = ggml_new_tensor_2d(g.ctx, GGML_TYPE_F32, cols, rows);
    ggml_tensor* out = ggml_soft_max(g.ctx, in);
    g.gf = ggml_new_graph(g.ctx);
    ggml_build_forward_expand(g.gf, out);
    if (auto r = impl_->finalize(g); !r)
        return r.error();
    ggml_backend_tensor_set(in, x.data(), 0, x.size() * sizeof(float));
    const auto check = [&](const std::vector<float>& got) {
        double worst = 0.0;
        for (std::uint32_t r : {0u, rows / 2, rows - 1}) {
            double mx = -1e300, sum = 0.0;
            for (std::uint32_t c = 0; c < cols; ++c) mx = std::max(mx, double(x[std::size_t{r} * cols + c]));
            for (std::uint32_t c = 0; c < cols; ++c)
                sum += std::exp(double(x[std::size_t{r} * cols + c]) - mx);
            for (std::uint32_t c = 0; c < cols; ++c) {
                const double ref = std::exp(double(x[std::size_t{r} * cols + c]) - mx) / sum;
                worst = std::max(worst, std::fabs(double(got[std::size_t{r} * cols + c]) - ref));
            }
        }
        return worst;  // probabilities sum to 1: absolute error is already scale-free
    };
    auto r = impl_->time_graph(g, out, samples, 6.0 * rows * double(cols), 1e-4, check);
    if (r)
        r.value().variant = "soft_max (scale 1, no mask)";
    return r;
}

Result<Measurement> Session::layernorm(std::uint32_t rows, std::uint32_t cols, float eps,
                                       std::uint32_t samples) {
    if (rows == 0 || cols == 0)
        return make_error(ErrorCode::InvalidArgument, "layernorm needs rows, cols > 0");
    const std::uint32_t seed = 0xB45E2002u ^ (rows * 31u + cols);
    const std::vector<float> x = rand_f32(std::size_t{rows} * cols, seed, -3.0f, 3.0f);
    const std::vector<float> gam = rand_f32(cols, seed + 1, 0.5f, 1.5f);
    const std::vector<float> bet = rand_f32(cols, seed + 2, -0.5f, 0.5f);
    Graph g;
    if (auto r = impl_->make_graph(g, 8); !r)
        return r.error();
    ggml_tensor* in = ggml_new_tensor_2d(g.ctx, GGML_TYPE_F32, cols, rows);
    ggml_tensor* tg = ggml_new_tensor_1d(g.ctx, GGML_TYPE_F32, cols);
    ggml_tensor* tb = ggml_new_tensor_1d(g.ctx, GGML_TYPE_F32, cols);
    ggml_tensor* out = ggml_add(g.ctx, ggml_mul(g.ctx, ggml_norm(g.ctx, in, eps), tg), tb);
    g.gf = ggml_new_graph(g.ctx);
    ggml_build_forward_expand(g.gf, out);
    if (auto r = impl_->finalize(g); !r)
        return r.error();
    ggml_backend_tensor_set(in, x.data(), 0, x.size() * sizeof(float));
    ggml_backend_tensor_set(tg, gam.data(), 0, gam.size() * sizeof(float));
    ggml_backend_tensor_set(tb, bet.data(), 0, bet.size() * sizeof(float));
    const auto check = [&](const std::vector<float>& got) {
        double worst = 0.0;
        for (std::uint32_t r : {0u, rows / 2, rows - 1}) {
            double mean = 0.0, var = 0.0;
            for (std::uint32_t c = 0; c < cols; ++c) mean += x[std::size_t{r} * cols + c];
            mean /= cols;
            for (std::uint32_t c = 0; c < cols; ++c)
                var += std::pow(x[std::size_t{r} * cols + c] - mean, 2.0);
            var /= cols;
            const double inv = 1.0 / std::sqrt(var + double(eps));
            double mx = 1e-6;
            std::vector<double> ref(cols);
            for (std::uint32_t c = 0; c < cols; ++c) {
                ref[c] = (x[std::size_t{r} * cols + c] - mean) * inv * gam[c] + bet[c];
                mx = std::max(mx, std::fabs(ref[c]));
            }
            for (std::uint32_t c = 0; c < cols; ++c)
                worst = std::max(worst, std::fabs(double(got[std::size_t{r} * cols + c]) - ref[c]) / mx);
        }
        return worst;
    };
    auto r = impl_->time_graph(g, out, samples, 8.0 * rows * double(cols), 1e-3, check);
    if (r)
        r.value().variant = "norm + mul(gamma) + add(beta)";
    return r;
}

Result<Measurement> Session::rmsnorm(std::uint32_t rows, std::uint32_t cols, float eps,
                                     std::uint32_t samples) {
    if (rows == 0 || cols == 0)
        return make_error(ErrorCode::InvalidArgument, "rmsnorm needs rows, cols > 0");
    const std::uint32_t seed = 0xB45E3003u ^ (rows * 31u + cols);
    const std::vector<float> x = rand_f32(std::size_t{rows} * cols, seed, -3.0f, 3.0f);
    const std::vector<float> gam = rand_f32(cols, seed + 1, 0.5f, 1.5f);
    Graph g;
    if (auto r = impl_->make_graph(g, 8); !r)
        return r.error();
    ggml_tensor* in = ggml_new_tensor_2d(g.ctx, GGML_TYPE_F32, cols, rows);
    ggml_tensor* tg = ggml_new_tensor_1d(g.ctx, GGML_TYPE_F32, cols);
    ggml_tensor* out = ggml_mul(g.ctx, ggml_rms_norm(g.ctx, in, eps), tg);
    g.gf = ggml_new_graph(g.ctx);
    ggml_build_forward_expand(g.gf, out);
    if (auto r = impl_->finalize(g); !r)
        return r.error();
    ggml_backend_tensor_set(in, x.data(), 0, x.size() * sizeof(float));
    ggml_backend_tensor_set(tg, gam.data(), 0, gam.size() * sizeof(float));
    const auto check = [&](const std::vector<float>& got) {
        double worst = 0.0;
        for (std::uint32_t r : {0u, rows / 2, rows - 1}) {
            double ms = 0.0;
            for (std::uint32_t c = 0; c < cols; ++c)
                ms += std::pow(double(x[std::size_t{r} * cols + c]), 2.0);
            ms /= cols;
            const double inv = 1.0 / std::sqrt(ms + double(eps));
            double mx = 1e-6;
            std::vector<double> ref(cols);
            for (std::uint32_t c = 0; c < cols; ++c) {
                ref[c] = x[std::size_t{r} * cols + c] * inv * gam[c];
                mx = std::max(mx, std::fabs(ref[c]));
            }
            for (std::uint32_t c = 0; c < cols; ++c)
                worst = std::max(worst, std::fabs(double(got[std::size_t{r} * cols + c]) - ref[c]) / mx);
        }
        return worst;
    };
    auto r = impl_->time_graph(g, out, samples, 4.0 * rows * double(cols), 1e-3, check);
    if (r)
        r.value().variant = "rms_norm + mul(gamma)";
    return r;
}

Result<Measurement> Session::rope(std::uint32_t rows, std::uint32_t dim, std::uint32_t pos0,
                                  std::uint32_t samples) {
    if (rows == 0 || dim < 2 || dim % 2 != 0)
        return make_error(ErrorCode::InvalidArgument, "rope needs rows > 0 and an even dim >= 2");
    const std::vector<float> x =
        rand_f32(std::size_t{rows} * dim, 0xB45E4004u ^ (rows * 31u + dim), -1.0f, 1.0f);
    std::vector<std::int32_t> p(rows);
    for (std::uint32_t i = 0; i < rows; ++i) p[i] = static_cast<std::int32_t>(pos0 + i);
    constexpr float kBase = 10000.0f;
    Graph g;
    if (auto r = impl_->make_graph(g, 8); !r)
        return r.error();
    ggml_tensor* in = ggml_new_tensor_3d(g.ctx, GGML_TYPE_F32, dim, 1, rows);
    ggml_tensor* tp = ggml_new_tensor_1d(g.ctx, GGML_TYPE_I32, rows);
    ggml_tensor* out = ggml_rope_ext(g.ctx, in, tp, nullptr, static_cast<int>(dim), GGML_ROPE_TYPE_NEOX, 4096,
                                     kBase, 1.0f, 0.0f, 1.0f, 32.0f, 1.0f);
    g.gf = ggml_new_graph(g.ctx);
    ggml_build_forward_expand(g.gf, out);
    if (auto r = impl_->finalize(g); !r)
        return r.error();
    ggml_backend_tensor_set(in, x.data(), 0, x.size() * sizeof(float));
    ggml_backend_tensor_set(tp, p.data(), 0, p.size() * sizeof(std::int32_t));
    const auto check = [&](const std::vector<float>& got) {
        double worst = 0.0;
        const std::size_t half = dim / 2;
        for (std::uint32_t r : {0u, rows / 2, rows - 1}) {
            for (std::size_t i = 0; i < half; ++i) {
                const double theta =
                    double(pos0 + r) * std::pow(double(kBase), -2.0 * double(i) / double(dim));
                const double a = x[std::size_t{r} * dim + i], b = x[std::size_t{r} * dim + i + half];
                const double y0 = a * std::cos(theta) - b * std::sin(theta);
                const double y1 = a * std::sin(theta) + b * std::cos(theta);
                worst = std::max(worst, std::fabs(double(got[std::size_t{r} * dim + i]) - y0));
                worst = std::max(worst, std::fabs(double(got[std::size_t{r} * dim + i + half]) - y1));
            }
        }
        return worst;  // unit-scale inputs: absolute error
    };
    auto r = impl_->time_graph(g, out, samples, 6.0 * rows * double(dim / 2), 5e-3, check);
    if (r)
        r.value().variant = "rope_ext (NEOX split-half, base 10000)";
    return r;
}

}  // namespace hcs::kernels::ggml_baseline
