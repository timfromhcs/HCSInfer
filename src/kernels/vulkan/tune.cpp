// `HCSInfer tune` (AGENTS.md 12.5 / 16.6): per kernel and shape bucket, check correctness against the CPU
// oracle, check 3 bit-identical runs, time the kernel (wall + GPU timestamps) and a multi-threaded CPU
// baseline, evaluate the promotion rule and persist the keyed profile cache.
//
// Honesty rules encoded here:
//  * a kernel the device cannot run is reported "NOT RUN on this hardware" with the reason (never skipped
//    silently, never faked);
//  * only entries that passed correctness are persisted; a failing kernel removes its own stale evidence;
//  * the upstream GGML Vulkan op baseline is measured in-process (hcs/kernels/ggml_vk_baseline.hpp: the
//  pinned
//    ggml backend, same shape, same device, same timing method) when this build has the ggml Vulkan backend;
//    otherwise it is recorded as NOT RUN. A kernel is default-enabled only when promoted AND measured
//    >= 1.05x faster than the upstream operator (planner: promoted != default-enabled);
//  * one dispatch whose first (cold) run takes > 1.5 s is abandoned: TDR guard, the OS watchdog resets a GPU
//    that is busy for ~2 s.
#include "hcs/kernels/vk_tune.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>

#include "hcs/fs_util.hpp"
#include "hcs/hardware/planner.hpp"
#include "hcs/kernels/cpu.hpp"
#include "hcs/kernels/cpu_baseline.hpp"
#ifdef HCS_HAVE_GGML_BASELINE
#include "hcs/kernels/ggml_vk_baseline.hpp"
#endif
#include "hcs/kernels/oracle.hpp"
#include "hcs/kernels/vk_ops.hpp"
#include "hcs/version.hpp"

namespace hcs::kernels::vulkan {

namespace {

using hardware::Shape;
using json = nlohmann::json;
namespace cpu = hcs::kernels::cpu;
using Clock = std::chrono::steady_clock;

constexpr double kColdAbortMs = 1500.0;

const char* kHelp =
    "Usage: HCSInfer tune [--device <index|name|radeon-680m>] [--suite quick|full|llm] [--kernel <id>]...\n"
    "                     [--samples N] [--out <evidence.json>] [--json] [--validation|--no-validation]\n"
    "                     [--no-upstream]\n"
    "       HCSInfer tune --show  [--device <sel>]       Show the planner table from the saved profile (no "
    "GPU work)\n"
    "       HCSInfer tune --reset [--device <sel>] [--kernel <id>]   Drop saved evidence for the device\n"
    "\n"
    "Benchmarks every HCS Vulkan kernel (tiled f32/f16 GEMM, int8-dot GEMM, cooperative-matrix GEMM, "
    "softmax,\n"
    "LayerNorm, RMSNorm, RoPE) on this machine: correctness vs the CPU oracle, 3 bit-identical runs, wall + "
    "GPU\n"
    "timestamp timings, a multi-threaded CPU baseline and - when this build has the ggml Vulkan backend - "
    "the\n"
    "upstream GGML Vulkan operator on the same shape (--no-upstream skips it). Suite llm adds real LLM and\n"
    "SD-like shapes. Results are stored in <state-dir>/kernel-profile.json, keyed by device, driver, shader "
    "hash\n"
    "and shape bucket. A kernel is PROMOTED only when correct, deterministic and >= 1.05x the CPU baseline;\n"
    "it is DEFAULT-ENABLED only with a measured upstream baseline that it beats by >= 1.05x. Run the whole\n"
    "tune three times (scripts/kernel-decision.ps1) before trusting a promotion. Nothing enables itself:\n"
    "the engines never call these kernels unless a feature flag says so.\n"
    "--reset drops the saved evidence (forces a fresh rebenchmark); stale evidence (other driver, other "
    "shader\n"
    "hash) is never matched anyway.\n"
    "Kernels the device cannot run are reported as 'NOT RUN on this hardware' with the reason.\n"
    "Exit codes: 0 ok, 2 invalid argument, 4 a kernel produced wrong results / device failure, 10 no Vulkan "
    "device.\n";

struct Options {
    std::string device;
    std::string suite = "quick";
    std::vector<std::string> kernels;
    std::uint32_t samples = 0;  // 0 = suite default
    bool no_upstream = false;
    std::string out_file;
    bool as_json = false;
    bool show = false;
    bool reset = false;
    int validation = -1;  // -1 default (debug builds on), 0 off, 1 on
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
double spread_of(const std::vector<double>& v) {
    const double m = median_of(v);
    return m > 0.0 ? (percentile_of(v, 0.9) - percentile_of(v, 0.1)) / m : 0.0;
}

// Deterministic generator so evidence is reproducible (documented seed per shape).
struct Rng {
    std::mt19937 g;
    explicit Rng(std::uint32_t seed) : g(seed) {}
    float f(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g); }
    std::int8_t i8() { return static_cast<std::int8_t>(std::uniform_int_distribution<int>(-128, 127)(g)); }
};
std::vector<float> rand_f32(std::size_t n, std::uint32_t seed, float lo, float hi) {
    Rng r(seed);
    std::vector<float> v(n);
    for (auto& x : v) x = r.f(lo, hi);
    return v;
}
std::vector<std::uint16_t> rand_f16(std::size_t n, std::uint32_t seed) {
    const auto f = rand_f32(n, seed, -1.0f, 1.0f);
    std::vector<std::uint16_t> h(n);
    for (std::size_t i = 0; i < n; ++i) h[i] = cpu::fp32_to_fp16(f[i]);
    return h;
}
std::vector<std::int8_t> rand_i8(std::size_t n, std::uint32_t seed) {
    Rng r(seed);
    std::vector<std::int8_t> v(n);
    for (auto& x : v) x = r.i8();
    return v;
}
std::vector<float> widen(const std::vector<std::uint16_t>& h) {
    std::vector<float> f(h.size());
    for (std::size_t i = 0; i < h.size(); ++i) f[i] = cpu::fp16_to_fp32(h[i]);
    return f;
}

struct CaseResult {
    std::string kernel, bucket, status, reason;
    Shape shape;
    bool have_entry = false;
    hardware::ProfileEntry entry;
    hardware::PromotionDecision decision;
    std::vector<std::string> validation_notes;
};

struct Suite {
    std::vector<Shape> gemm, rows, rope;
};
Suite suite_for(const std::string& name) {
    Suite s;
    if (name == "full" || name == "llm") {
        s.gemm = {{128, 128, 128}, {256, 1024, 256}, {512, 2048, 512}, {1024, 1024, 1024}};
        s.rows = {{32, 4096, 0}, {256, 1024, 0}, {4, 32000, 0}};
        s.rope = {{512, 128, 0}, {2048, 128, 0}};
        if (name == "llm") {
            // Real shapes (m = tokens, n = output features, k = input features), ascending so the TDR guard
            // can predict the duration of the big ones from the rate measured on the small ones:
            //   decode GEMV 1x1024x3072 / 1x4096x4096 / 1x14336x4096 (qwen3-0.6b FFN, Llama-8B proj/FFN),
            //   prefill 128x4096x4096 and 512x4096x4096, SD UNet-like: attention QK^T 1024x1024x64 and
            //   3x3 conv as GEMM at 32x32 latent 1024x320x2880 and 1x1 conv / attention projection
            //   4096x320x320.
            s.gemm.insert(s.gemm.end(), {{1, 1024, 3072},
                                         {1, 4096, 4096},
                                         {1, 14336, 4096},
                                         {1024, 1024, 64},
                                         {4096, 320, 320},
                                         {1024, 320, 2880},
                                         {128, 4096, 4096},
                                         {512, 4096, 4096}});
            s.rows.insert(s.rows.end(), {{1024, 1024, 0}, {64, 14336, 0}});
            s.rope.insert(s.rope.end(), {{1, 128, 0}, {4096, 128, 0}});
        }
    } else {
        s.gemm = {{256, 1024, 256}};
        s.rows = {{32, 4096, 0}};
        s.rope = {{512, 128, 0}};
    }
    return s;
}
std::vector<Shape> shapes_for(const Suite& s, hardware::Op op) {
    switch (op) {
        case hardware::Op::Gemm:
            return s.gemm;
        case hardware::Op::Rope:
            return s.rope;
        default:
            return s.rows;
    }
}

template <class Fn>
double time_ms(Fn&& fn) {
    const auto t0 = Clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// The per-kernel pieces the generic benchmark loop needs.
struct Prepared {
    PreparedOp op;
    bool is_int = false;
    std::vector<float> want_f;
    std::vector<std::int32_t> want_i;
    cpu::Tolerance tol;
    std::function<void()> baseline;  // runs the multi-threaded CPU baseline once
    std::string baseline_name = "cpu_mt_autovec";
};

Result<Prepared> prepare_case(KernelSet& ks, const std::string& id, const Shape& s, std::uint32_t seed) {
    Prepared p;
    const auto m = static_cast<std::uint32_t>(s.m), n = static_cast<std::uint32_t>(s.n),
               k = static_cast<std::uint32_t>(s.k);
    const float eps = 1e-5f;
    if (id == "gemm_f32_tiled") {
        auto a = std::make_shared<std::vector<float>>(rand_f32(std::size_t{m} * k, seed, -1, 1));
        auto b = std::make_shared<std::vector<float>>(rand_f32(std::size_t{k} * n, seed + 1, -1, 1));
        auto op = ks.prepare_gemm_f32(a->data(), b->data(), m, n, k);
        if (!op)
            return op.error();
        p.op = std::move(op.value());
        p.want_f.resize(std::size_t{m} * n);
        cpu::gemm_f32(a->data(), b->data(), p.want_f.data(), m, n, k);
        p.tol = cpu::tolerance_f32().for_len(k);
        auto out = std::make_shared<std::vector<float>>(std::size_t{m} * n);
        p.baseline = [=] { cpu::baseline_gemm_f32(a->data(), b->data(), out->data(), m, n, k); };
    } else if (id == "gemm_f16_tiled" || id == "gemm_coopmat_f16") {
        auto a = std::make_shared<std::vector<std::uint16_t>>(rand_f16(std::size_t{m} * k, seed));
        auto b = std::make_shared<std::vector<std::uint16_t>>(rand_f16(std::size_t{k} * n, seed + 1));
        auto op = id == "gemm_f16_tiled" ? ks.prepare_gemm_f16(a->data(), b->data(), m, n, k)
                                         : ks.prepare_gemm_coopmat_f16(a->data(), b->data(), m, n, k);
        if (!op)
            return op.error();
        p.op = std::move(op.value());
        p.want_f.resize(std::size_t{m} * n);
        cpu::gemm_f16(a->data(), b->data(), p.want_f.data(), m, n, k);
        p.tol = cpu::tolerance_f32().for_len(k);  // halves are widened to f32 before the math
        // CPU baseline works on pre-widened floats (conversion excluded: favours the CPU, i.e. is
        // conservative).
        auto af = std::make_shared<std::vector<float>>(widen(*a));
        auto bf = std::make_shared<std::vector<float>>(widen(*b));
        auto out = std::make_shared<std::vector<float>>(std::size_t{m} * n);
        p.baseline = [=] { cpu::baseline_gemm_f32(af->data(), bf->data(), out->data(), m, n, k); };
    } else if (id == "gemm_i8dot_tiled") {
        auto a = std::make_shared<std::vector<std::int8_t>>(rand_i8(std::size_t{m} * k, seed));
        auto b = std::make_shared<std::vector<std::int8_t>>(rand_i8(std::size_t{k} * n, seed + 1));
        auto op = ks.prepare_gemm_i8dot(a->data(), b->data(), m, n, k);
        if (!op)
            return op.error();
        p.op = std::move(op.value());
        p.is_int = true;
        p.want_i.resize(std::size_t{m} * n);
        cpu::gemm_i8(a->data(), b->data(), p.want_i.data(), m, n, k);
        p.tol = cpu::tolerance_exact();
        auto out = std::make_shared<std::vector<std::int32_t>>(std::size_t{m} * n);
        p.baseline = [=] { cpu::baseline_gemm_i8(a->data(), b->data(), out->data(), m, n, k); };
    } else if (id == "softmax_rows") {
        auto x = std::make_shared<std::vector<float>>(rand_f32(std::size_t{m} * n, seed, -8, 8));
        auto op = ks.prepare_softmax(x->data(), m, n);
        if (!op)
            return op.error();
        p.op = std::move(op.value());
        p.want_f.resize(std::size_t{m} * n);
        cpu::softmax_rows(x->data(), p.want_f.data(), m, n);
        p.tol = cpu::tolerance_f32().for_len(n);
        auto out = std::make_shared<std::vector<float>>(std::size_t{m} * n);
        p.baseline = [=] { cpu::baseline_softmax_rows(x->data(), out->data(), m, n); };
    } else if (id == "layernorm_rows") {
        auto x = std::make_shared<std::vector<float>>(rand_f32(std::size_t{m} * n, seed, -3, 3));
        auto g = std::make_shared<std::vector<float>>(rand_f32(n, seed + 1, 0.5f, 1.5f));
        auto b = std::make_shared<std::vector<float>>(rand_f32(n, seed + 2, -0.5f, 0.5f));
        auto op = ks.prepare_layernorm(x->data(), g->data(), b->data(), m, n, eps);
        if (!op)
            return op.error();
        p.op = std::move(op.value());
        p.want_f.resize(std::size_t{m} * n);
        cpu::layernorm_rows(x->data(), g->data(), b->data(), p.want_f.data(), m, n, eps);
        p.tol = cpu::tolerance_f32().for_len(n);
        auto out = std::make_shared<std::vector<float>>(std::size_t{m} * n);
        p.baseline = [=] {
            cpu::baseline_layernorm_rows(x->data(), g->data(), b->data(), out->data(), m, n, eps);
        };
    } else if (id == "rmsnorm_rows") {
        auto x = std::make_shared<std::vector<float>>(rand_f32(std::size_t{m} * n, seed, -3, 3));
        auto g = std::make_shared<std::vector<float>>(rand_f32(n, seed + 1, 0.5f, 1.5f));
        auto op = ks.prepare_rmsnorm(x->data(), g->data(), m, n, eps);
        if (!op)
            return op.error();
        p.op = std::move(op.value());
        p.want_f.resize(std::size_t{m} * n);
        cpu::rmsnorm_rows(x->data(), g->data(), p.want_f.data(), m, n, eps);
        p.tol = cpu::tolerance_f32().for_len(n);
        auto out = std::make_shared<std::vector<float>>(std::size_t{m} * n);
        p.baseline = [=] { cpu::baseline_rmsnorm_rows(x->data(), g->data(), out->data(), m, n, eps); };
    } else if (id == "rope_rows") {
        auto x = std::make_shared<std::vector<float>>(rand_f32(std::size_t{m} * n, seed, -1, 1));
        auto op = ks.prepare_rope(x->data(), m, n, 0, 10000.0f);
        if (!op)
            return op.error();
        p.op = std::move(op.value());
        p.want_f.resize(std::size_t{m} * n);
        cpu::rope_rows(x->data(), p.want_f.data(), m, n, 0, 10000.0f);
        // Vulkan only bounds sin/cos to 2^-11 inside [-pi, pi]; angles here reach `rows` rad. Declared
        // absolute tolerance 1e-3 for unit-scale inputs (documented in docs/vulkan-kernels.md); the measured
        // max error is recorded next to it.
        p.tol = cpu::Tolerance{1e-5, 1e-3};
        auto out = std::make_shared<std::vector<float>>(std::size_t{m} * n);
        p.baseline = [=] { cpu::baseline_rope_rows(x->data(), out->data(), m, n, 0, 10000.0f); };
    } else {
        return make_error(ErrorCode::InvalidArgument, "unknown kernel " + id);
    }
    return p;
}

// The in-process upstream GGML Vulkan baseline for one tune run (see hcs/kernels/ggml_vk_baseline.hpp).
struct UpstreamCtx {
#ifdef HCS_HAVE_GGML_BASELINE
    std::optional<ggml_baseline::Session> session;
    std::string device_description;
    std::string state = "NOT RUN";
#else
    std::string state =
        "NOT RUN (this build has no ggml Vulkan backend: needs HCS_ENABLE_VULKAN=ON and "
        "external/llama.cpp)";
#endif
    bool available() const {
#ifdef HCS_HAVE_GGML_BASELINE
        return session.has_value();
#else
        return false;
#endif
    }
    void init(const std::string& device_name, bool disabled) {
        if (disabled) {
            state = "NOT RUN (--no-upstream)";
            return;
        }
#ifdef HCS_HAVE_GGML_BASELINE
        auto r = ggml_baseline::Session::create(device_name);
        if (!r) {
            state = "NOT RUN (cannot create the ggml Vulkan session: " + r.error().message + ")";
            return;
        }
        session.emplace(std::move(r.value()));
        device_description = session->device().description;
        state = "measured in-process with the pinned ggml Vulkan backend on '" + device_description + "'";
#else
        (void)device_name;
#endif
    }
};

#ifdef HCS_HAVE_GGML_BASELINE
Result<ggml_baseline::Measurement> upstream_op(ggml_baseline::Session& s, const std::string& id,
                                               const Shape& sh, std::uint32_t samples) {
    const auto m = static_cast<std::uint32_t>(sh.m), n = static_cast<std::uint32_t>(sh.n),
               k = static_cast<std::uint32_t>(sh.k);
    if (id == "gemm_f32_tiled")
        return s.mul_mat(ggml_baseline::WeightType::F32, m, n, k, samples);
    if (id == "gemm_f16_tiled" || id == "gemm_coopmat_f16")
        return s.mul_mat(ggml_baseline::WeightType::F16, m, n, k, samples);
    if (id == "softmax_rows")
        return s.softmax(m, n, samples);
    if (id == "layernorm_rows")
        return s.layernorm(m, n, 1e-5f, samples);
    if (id == "rmsnorm_rows")
        return s.rmsnorm(m, n, 1e-5f, samples);
    if (id == "rope_rows")
        return s.rope(m, n, 0, samples);
    return make_error(ErrorCode::InvalidArgument, "no upstream mapping for " + id);
}
#endif

void measure_upstream(UpstreamCtx& up, const std::string& id, const Shape& sh, std::uint32_t samples,
                      hardware::ProfileEntry& e) {
    if (id == "gemm_i8dot_tiled") {
        // GGML has no int8 x int8 -> int32 mul_mat: Q8_0 weights x f32 activations (quantised to Q8_1 inside)
        // is a different operation. Not compared, so it can never be default-enabled on this evidence.
        e.upstream_status = "not_comparable";
        e.upstream_note =
            "no GGML operator computes int8 x int8 -> int32 (closest: Q8_0 mul_mat with f32 "
            "activations, different semantics)";
        return;
    }
    if (!up.available()) {
        e.upstream_status = "not_run";
        e.upstream_note = up.state;
        return;
    }
#ifdef HCS_HAVE_GGML_BASELINE
    auto r = upstream_op(*up.session, id, sh, samples);
    if (!r) {
        e.upstream_status = "error";
        e.upstream_note = r.error().message;
        return;
    }
    const ggml_baseline::Measurement& m = r.value();
    e.upstream_status = "measured";
    e.upstream_ms = m.median_ms;
    e.upstream_spread = m.spread;
    e.upstream_variant = m.variant;
    e.upstream_note = m.note;
    e.upstream_ratio = e.wall_ms_median > 0.0 ? m.median_ms / e.wall_ms_median : 0.0;
#else
    (void)sh;
    (void)samples;
#endif
}

CaseResult run_case(Runner& runner, KernelSet& ks, const hardware::KernelDesc& kd, const Shape& s,
                    std::uint32_t samples, const std::string& hash, UpstreamCtx& up,
                    std::map<std::string, double>& rates_gflops) {
    const hardware::DeviceCaps& caps = runner.caps();
    CaseResult r;
    r.kernel = kd.id;
    r.shape = s;
    r.bucket = hardware::shape_bucket(s);

    const double flops_guess =
        s.k > 0 ? 2.0 * static_cast<double>(s.m) * static_cast<double>(s.n) * static_cast<double>(s.k)
                : 6.0 * static_cast<double>(s.m) * static_cast<double>(s.n);
    // TDR guard (the OS watchdog resets a GPU that is busy for ~2 s). Without a measured rate for this kernel
    // the absolute cap is 4 GFLOP (1.5 s at a 3 GFLOP/s floor); once a smaller shape of the same kernel has
    // been timed, the guard predicts the new dispatch from that rate and allows up to 0.5 s.
    const auto known = rates_gflops.find(kd.id);
    if (known != rates_gflops.end() && known->second > 0.0) {
        const double predicted_ms = flops_guess / (known->second * 1e9) * 1e3;
        if (predicted_ms > 500.0) {
            r.status = "SKIPPED";
            r.reason = "TDR guard: predicted " + std::to_string(predicted_ms) +
                       " ms for one dispatch at the "
                       "rate measured on smaller shapes";
            return r;
        }
    } else if (flops_guess > 4.0e9) {
        r.status = "SKIPPED";
        r.reason = "TDR guard: shape too large for a single dispatch";
        return r;
    }
    auto prep = prepare_case(
        ks, kd.id, s,
        static_cast<std::uint32_t>(0x5EED0000u ^ static_cast<std::uint32_t>(s.m * 31 + s.n * 7 + s.k)));
    if (!prep) {
        r.status = prep.error().code == ErrorCode::Unsupported ? "NOT RUN on this hardware" : "ERROR";
        r.reason = prep.error().message;
        return r;
    }
    Prepared& p = prep.value();
    const std::size_t out_bytes = p.op.output_bytes();

    // --- correctness + determinism: 3 runs, each compared with the oracle and bit-compared with run 1
    // -------
    constexpr std::uint32_t kRuns = 3;
    std::vector<unsigned char> first(out_bytes), cur(out_bytes);
    std::uint32_t identical = 0;
    cpu::Comparison cmp{};
    for (std::uint32_t i = 0; i < kRuns; ++i) {
        auto t = p.op.run();
        if (!t) {
            r.status = "ERROR";
            r.reason = "dispatch failed: " + t.error().message;
            return r;
        }
        if (i == 0 && t.value().wall_ms > kColdAbortMs) {
            r.status = "SKIPPED";
            r.reason = "TDR guard: first dispatch took " + std::to_string(t.value().wall_ms) + " ms";
            return r;
        }
        if (auto rd = p.op.read_output(cur.data(), out_bytes); !rd) {
            r.status = "ERROR";
            r.reason = "readback failed: " + rd.error().message;
            return r;
        }
        if (i == 0) {
            first = cur;
            if (p.is_int) {
                std::vector<std::int32_t> got(p.want_i.size());
                std::memcpy(got.data(), cur.data(), got.size() * sizeof(std::int32_t));
                cmp = cpu::compare_exact(p.want_i.data(), got.data(), got.size());
            } else {
                std::vector<float> got(p.want_f.size());
                std::memcpy(got.data(), cur.data(), got.size() * sizeof(float));
                cmp = cpu::compare(p.want_f.data(), got.data(), got.size(), p.tol);
            }
        }
        if (std::memcmp(first.data(), cur.data(), out_bytes) == 0)
            ++identical;
    }

    hardware::ProfileEntry& e = r.entry;
    e.device_key = caps.device_key();
    e.kernel_id = kd.id;
    e.kernel_hash = hash;
    e.shape_bucket = r.bucket;
    e.device_name = caps.name;
    e.driver_info = caps.driver_info.empty() ? caps.driver_version_raw : caps.driver_info;
    e.m = s.m;
    e.n = s.n;
    e.k = s.k;
    e.hcs_commit = HCS_GIT_COMMIT;
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
        e.timestamp_utc = buf;
    }
    e.correct = cmp.ok;
    e.max_abs_error = cmp.max_abs_error;
    e.tol_rel = p.tol.rel;
    e.tol_abs = p.tol.abs;
    e.identical_runs = identical;
    e.total_runs = kRuns;
    e.flops = p.op.flops();
    r.have_entry = true;
    if (!cmp.ok) {
        r.status = "FAIL (wrong result)";
        std::ostringstream os;
        os << cmp.mismatches << " of " << cmp.count << " elements outside tolerance, max abs error "
           << cmp.max_abs_error << " (worst index " << cmp.worst_index << ")";
        r.reason = os.str();
        r.decision = hardware::evaluate_promotion(e);
        return r;
    }

    // --- timing: warm-up (clock ramp), then single dispatches
    // ------------------------------------------------
    {
        const auto t0 = Clock::now();
        int n = 0;
        while ((n < 10 || std::chrono::duration<double, std::milli>(Clock::now() - t0).count() < 250.0) &&
               n < 400) {
            auto t = p.op.run();
            if (!t) {
                r.status = "ERROR";
                r.reason = "dispatch failed during warm-up: " + t.error().message;
                r.have_entry = false;
                return r;
            }
            ++n;
        }
    }
    std::vector<double> wall, gpu;
    for (std::uint32_t i = 0; i < samples; ++i) {
        auto t = p.op.run();
        if (!t) {
            r.status = "ERROR";
            r.reason = "dispatch failed while timing: " + t.error().message;
            r.have_entry = false;
            return r;
        }
        wall.push_back(t.value().wall_ms);
        if (t.value().gpu_ms)
            gpu.push_back(*t.value().gpu_ms);
    }
    e.samples = samples;
    e.wall_ms_median = median_of(wall);
    e.wall_spread = spread_of(wall);
    if (gpu.size() == wall.size() && !gpu.empty()) {
        e.gpu_ms_median = median_of(gpu);
        e.gpu_spread = spread_of(gpu);
    }

    // --- CPU baseline -------------------------------------------------------------------------------------
    p.baseline();  // warm-up
    std::vector<double> base;
    for (std::uint32_t i = 0; i < std::max<std::uint32_t>(5, samples / 2); ++i)
        base.push_back(time_ms(p.baseline));
    e.baseline_name = p.baseline_name + " (" +
                      std::to_string(std::max(1u, std::thread::hardware_concurrency())) + " threads)";
    e.baseline_ms = median_of(base);
    e.upstream_status = "not_run";
    if (const auto gf = e.gflops_wall())
        rates_gflops[kd.id] = std::max(rates_gflops[kd.id], *gf * 0.5);  // keep a conservative (halved) rate
    measure_upstream(up, kd.id, s, samples, e);

    r.decision = hardware::evaluate_promotion(e);
    r.status = r.decision.default_enabled ? "PASS (default-enabled)"
               : r.decision.promoted      ? "PASS (promoted, not default-enabled)"
                                          : "PASS (correct, not promoted)";
    if (!r.decision.reasons.empty())
        r.reason = r.decision.reasons.front();
    return r;
}

json entry_json(const hardware::ProfileEntry& e, const hardware::PromotionDecision& d) {
    json j;
    j["shape"] = {{"m", e.m}, {"n", e.n}, {"k", e.k}};
    j["bucket"] = e.shape_bucket;
    j["kernel_hash"] = e.kernel_hash;
    j["correct"] = e.correct;
    j["max_abs_error"] = e.max_abs_error;
    j["tolerance"] = {{"rel", e.tol_rel}, {"abs", e.tol_abs}};
    j["identical_runs"] = e.identical_runs;
    j["total_runs"] = e.total_runs;
    j["samples"] = e.samples;
    j["wall_ms_median"] = e.wall_ms_median;
    j["wall_spread_p10_p90_over_median"] = e.wall_spread;
    j["gpu_ms_median"] = e.gpu_ms_median ? json(*e.gpu_ms_median) : json("unknown");
    j["gpu_spread_p10_p90_over_median"] = e.gpu_spread ? json(*e.gpu_spread) : json("unknown");
    j["flops"] = e.flops;
    j["gflops_wall"] = e.gflops_wall() ? json(*e.gflops_wall()) : json("unknown");
    j["gflops_gpu"] = e.gflops_gpu() ? json(*e.gflops_gpu()) : json("unknown");
    j["baseline"] = {{"name", e.baseline_name}, {"median_ms", e.baseline_ms}};
    j["speedup_vs_baseline"] = e.speedup();
    j["upstream_ggml_vulkan"] = {
        {"status", e.upstream_status},
        {"median_ms", e.upstream_ms},
        {"spread_p10_p90_over_median", e.upstream_spread},
        {"variant", e.upstream_variant},
        {"note", e.upstream_note},
        {"ratio_upstream_over_hcs", e.upstream_ratio ? json(*e.upstream_ratio) : json(nullptr)}};
    j["promoted"] = d.promoted;
    j["default_enabled"] = d.default_enabled;
    j["reasons"] = d.reasons;
    j["timestamp_utc"] = e.timestamp_utc;
    return j;
}

std::string fmt(double v, int prec = 3) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

bool parse_args(std::vector<std::string>& args, Options& o, std::string& error) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](std::string& dst) {
            if (i + 1 >= args.size()) {
                error = a + " needs a value";
                return false;
            }
            dst = args[++i];
            return true;
        };
        if (a == "--device") {
            if (!need(o.device))
                return false;
        } else if (a == "--suite") {
            if (!need(o.suite))
                return false;
            if (o.suite != "quick" && o.suite != "full" && o.suite != "llm") {
                error = "--suite must be quick, full or llm";
                return false;
            }
        } else if (a == "--kernel") {
            std::string k;
            if (!need(k))
                return false;
            if (hardware::find_kernel(k) == nullptr) {
                error = "unknown kernel id '" + k + "'";
                return false;
            }
            o.kernels.push_back(k);
        } else if (a == "--samples") {
            std::string v;
            if (!need(v))
                return false;
            if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos || v.size() > 4 ||
                std::stoul(v) < 3 || std::stoul(v) > 1000) {
                error = "--samples needs an integer in 3..1000";
                return false;
            }
            o.samples = static_cast<std::uint32_t>(std::stoul(v));
        } else if (a == "--out") {
            if (!need(o.out_file))
                return false;
        } else if (a == "--json") {
            o.as_json = true;
        } else if (a == "--show") {
            o.show = true;
        } else if (a == "--reset") {
            o.reset = true;
        } else if (a == "--no-upstream") {
            o.no_upstream = true;
        } else if (a == "--validation") {
            o.validation = 1;
        } else if (a == "--no-validation") {
            o.validation = 0;
        } else {
            error = "unexpected argument: " + a;
            return false;
        }
    }
    return true;
}

}  // namespace

const char* tune_help_text() {
    return kHelp;
}

int run_tune(std::vector<std::string> args, const std::filesystem::path& state_dir, std::ostream& out,
             std::ostream& err) {
    if (std::find(args.begin(), args.end(), "--help") != args.end() ||
        std::find(args.begin(), args.end(), "-h") != args.end()) {
        out << kHelp;
        return 0;
    }
    Options o;
    std::string perr;
    if (!parse_args(args, o, perr)) {
        err << "error: " << perr << " [invalid_argument]\n";
        return exit_code(ErrorCode::InvalidArgument);
    }
    std::string status, reason;
    const auto devices = hardware::probe_vulkan_caps(&status, &reason);
    if (devices.empty()) {
        err << "error: no Vulkan device available (" << status << (reason.empty() ? "" : ": " + reason)
            << ") - the CPU path is the only path on this machine [unsupported]\n";
        return exit_code(ErrorCode::Unsupported);
    }
    const auto sel = hardware::select_device(devices, o.device);
    if (!sel) {
        err << "error: no Vulkan device matches '" << o.device << "' [invalid_argument]\n";
        return exit_code(ErrorCode::InvalidArgument);
    }
    const hardware::DeviceCaps& caps = devices[*sel];
    const hardware::KernelHashes hashes = kernel_hashes();
    const auto ppath = hardware::profile_path(state_dir);

    auto loaded = hardware::load_profile(ppath);
    if (!loaded) {
        err << "error: kernel profile " << ppath.string() << " is unreadable: " << loaded.error().message
            << " (it is not replaced automatically; fix or delete it) [" << to_string(loaded.error().code)
            << "]\n";
        return loaded.error().exit_code();
    }
    hardware::KernelProfile profile = std::move(loaded.value());

    if (o.reset) {
        const std::string prefix = "vk:" + [&] {
            char b[16];
            std::snprintf(b, sizeof(b), "%04x:%04x", caps.vendor_id, caps.device_id);
            return std::string(b);
        }();
        std::size_t n = 0;
        if (o.kernels.empty())
            n = profile.reset(prefix);
        else
            for (const auto& k : o.kernels) n += profile.reset(prefix, k);
        if (auto sv = hardware::save_profile(ppath, profile); !sv) {
            err << "error: cannot write " << ppath.string() << ": " << sv.error().message << "\n";
            return sv.error().exit_code();
        }
        out << "removed " << n << " profile entries for " << caps.name << " (" << prefix << ") from "
            << ppath.string() << "\n";
        return 0;
    }

    // Plan table from the saved profile (also printed after a run).
    auto print_plans = [&](std::ostream& os, const hardware::KernelProfile& prof) {
        static const struct {
            hardware::Op op;
            hardware::DType dt;
            Shape sh;
        } reps[] = {{hardware::Op::Gemm, hardware::DType::F32, {256, 1024, 256}},
                    {hardware::Op::Gemm, hardware::DType::F16, {256, 1024, 256}},
                    {hardware::Op::Gemm, hardware::DType::I8, {256, 1024, 256}},
                    {hardware::Op::Softmax, hardware::DType::F32, {32, 4096, 0}},
                    {hardware::Op::LayerNorm, hardware::DType::F32, {32, 4096, 0}},
                    {hardware::Op::RmsNorm, hardware::DType::F32, {32, 4096, 0}},
                    {hardware::Op::Rope, hardware::DType::F32, {512, 128, 0}}};
        os << "\nPlanner (release policy: unknown = CPU; needs promotion + measured upstream baseline):\n";
        for (const auto& r : reps) {
            const auto p = hardware::plan(r.op, r.dt, r.sh, caps, prof, hashes);
            os << "  " << std::left << std::setw(10) << hardware::to_string(r.op) << std::setw(5)
               << hardware::to_string(r.dt) << std::setw(20) << hardware::shape_bucket(r.sh)
               << hardware::to_string(p.kind)
               << (p.candidate_kernel_id.empty()
                       ? ""
                       : "  [candidate " + p.candidate_kernel_id + ": " + p.candidate_status + "]")
               << "\n      " << p.reason << "\n";
        }
    };

    if (o.show) {
        if (o.as_json) {
            json j;
            j["device"] = {{"name", caps.name}, {"device_key", caps.device_key()}};
            j["profile_path"] = ppath.string();
            j["entries"] = json::array();
            for (const auto& e : profile.entries())
                if (e.device_key == caps.device_key())
                    j["entries"].push_back(entry_json(e, hardware::evaluate_promotion(e)));
            out << j.dump(2) << "\n";
        } else {
            out << "Device: " << caps.name << "  (" << caps.device_key() << ")\nProfile: " << ppath.string()
                << "  (" << profile.entries().size() << " entries)\n";
            for (const auto& e : profile.entries()) {
                if (e.device_key != caps.device_key())
                    continue;
                const auto d = hardware::evaluate_promotion(e);
                out << "  " << e.kernel_id << " " << e.shape_bucket << "  speedup " << fmt(e.speedup(), 2)
                    << "x vs " << e.baseline_name << "  "
                    << (d.default_enabled ? "default-enabled"
                        : d.promoted      ? "promoted"
                                          : "not promoted")
                    << "\n";
            }
            print_plans(out, profile);
        }
        return 0;
    }

    // ---- benchmark run
    // --------------------------------------------------------------------------------------
    RunnerOptions ro;
    ro.device_index = caps.probe_index;
#ifndef NDEBUG
    ro.validation = o.validation != 0;
#else
    ro.validation = o.validation == 1;
#endif
    auto rr = Runner::create(ro);
    if (!rr) {
        err << "error: cannot create the Vulkan runner: " << rr.error().message << " ["
            << to_string(rr.error().code) << "]\n";
        return rr.error().exit_code();
    }
    Runner& runner = rr.value();
    KernelSet ks(runner);
    const Suite suite = suite_for(o.suite);
    const std::uint32_t samples = o.samples != 0 ? o.samples : (o.suite == "full" ? 21 : 15);

    UpstreamCtx upstream;
    upstream.init(caps.name, o.no_upstream);
    std::map<std::string, double> rates_gflops;
    std::vector<CaseResult> results;
    bool any_wrong = false, any_error = false;
    for (const auto& kd : hardware::kernel_catalog()) {
        if (!o.kernels.empty() &&
            std::find(o.kernels.begin(), o.kernels.end(), std::string(kd.id)) == o.kernels.end())
            continue;
        const hardware::Gate g = ks.gate(kd.id);
        if (!g.eligible) {
            CaseResult c;
            c.kernel = kd.id;
            c.status = "NOT RUN on this hardware";
            c.reason = g.reason;
            results.push_back(std::move(c));
            continue;
        }
        const auto h = hashes.find(kd.id);
        for (const Shape& s : shapes_for(suite, kd.op)) {
            CaseResult c = run_case(runner, ks, kd, s, samples, h != hashes.end() ? h->second : std::string(),
                                    upstream, rates_gflops);
            if (c.status.rfind("FAIL", 0) == 0) {
                any_wrong = true;
                // The old evidence for this key is now untrustworthy.
                profile.remove(c.entry.device_key, c.entry.kernel_id, c.entry.kernel_hash,
                               c.entry.shape_bucket);
            } else if (c.status == "ERROR") {
                any_error = true;
            } else if (c.have_entry && c.entry.correct) {
                if (auto up = profile.upsert(c.entry); !up)
                    c.reason += " (not persisted: " + up.error().message + ")";
            }
            results.push_back(std::move(c));
            if (runner.lost()) {  // device lost / timeout: stop, report, try the recovery path once
                if (auto rs = runner.reset(); !rs) {
                    any_error = true;
                    break;
                }
                ks = KernelSet(runner);
            }
        }
    }
    // Entries of kernels whose shader hash changed are dead weight for this device: drop them.
    {
        std::vector<std::pair<std::string, std::string>> live(hashes.begin(), hashes.end());
        profile.prune_stale(caps.device_key(), live);
    }
    if (auto sv = hardware::save_profile(ppath, profile); !sv) {
        err << "error: cannot write " << ppath.string() << ": " << sv.error().message << "\n";
        return sv.error().exit_code();
    }

    const ValidationReport vr = runner.validation();

    // ---- report
    // ---------------------------------------------------------------------------------------------
    json ev;
    ev["schema_version"] = 1;
    ev["generator"] = "HCSInfer tune";
    ev["hcsinfer_version"] = HCS_VERSION_STRING;
    ev["git_commit"] = HCS_GIT_COMMIT;
    ev["build_type"] = HCS_BUILD_TYPE;
    ev["suite"] = o.suite;
    ev["samples"] = samples;
    ev["device"] = {{"name", caps.name},
                    {"vendor_id", caps.vendor_id},
                    {"device_id", caps.device_id},
                    {"device_type", caps.device_type},
                    {"api_version", caps.api_version},
                    {"driver_version_raw", caps.driver_version_raw},
                    {"driver_name", caps.driver_name.empty() ? "unknown" : caps.driver_name},
                    {"driver_info", caps.driver_info.empty() ? "unknown" : caps.driver_info},
                    {"subgroup_size", caps.subgroup_size ? json(*caps.subgroup_size) : json("unknown")},
                    {"device_key", caps.device_key()}};
    ev["memory_placement"] = runner.device_local_buffers() ? "device_local (staged uploads)" : "host_visible";
    ev["timestamps"] = runner.timestamps_available();
    ev["cpu_threads"] = std::max(1u, std::thread::hardware_concurrency());
    ev["baseline"] =
        "cpu_mt_autovec: row-parallel std::thread, float accumulation, compiler auto-vectorisation only; "
        "conversion of f16 inputs excluded from the CPU timing";
    ev["upstream_ggml_vulkan_baseline"] = upstream.state;
    ev["upstream_method"] =
        "ggml_backend_graph_compute (submit + wait) of a one-node graph per operation, inputs device "
        "resident, "
        "warm-up then N timed samples; the same wall-clock method as the HCS number (one submit + fence)";
    ev["decisive_time"] = "wall_ms_median of one dispatch (submit + fence, data resident on the device)";
    ev["validation"] = {{"requested", vr.requested},
                        {"active", vr.active},
                        {"warning_and_error_messages", vr.messages},
                        {"first_messages", vr.first_messages},
                        {"note", vr.note}};
    ev["profile_path"] = ppath.string();
    json rj = json::array();
    for (const auto& c : results) {
        json j;
        j["kernel"] = c.kernel;
        j["status"] = c.status;
        j["reason"] = c.reason;
        if (c.have_entry)
            j.update(entry_json(c.entry, c.decision));
        rj.push_back(std::move(j));
    }
    ev["results"] = rj;

    if (!o.out_file.empty()) {
        if (auto w = atomic_write_file(o.out_file, ev.dump(2) + "\n"); !w) {
            err << "error: cannot write " << o.out_file << ": " << w.error().message << "\n";
            return w.error().exit_code();
        }
    }
    if (o.as_json) {
        out << ev.dump(2) << "\n";
    } else {
        out << "HCSInfer tune - " << caps.name << " (" << caps.device_key() << ")\n"
            << "driver: " << (caps.driver_info.empty() ? caps.driver_version_raw : caps.driver_info)
            << "   buffers: " << ev["memory_placement"].get<std::string>() << "   suite: " << o.suite << " ("
            << samples << " samples)\n"
            << "validation layer: "
            << (vr.requested ? (vr.active ? std::to_string(vr.messages) + " warning/error messages"
                                          : "NOT RUN (" + vr.note + ")")
                             : std::string("not requested"))
            << "\n\n";
        out << std::left << std::setw(18) << "kernel" << std::setw(15) << "bucket" << std::setw(11)
            << "max err" << std::setw(5) << "det" << std::setw(10) << "wall ms" << std::setw(10) << "gpu ms"
            << std::setw(10) << "GFLOP/s" << std::setw(10) << "cpu ms" << std::setw(9) << "vs cpu"
            << std::setw(10) << "ups ms" << std::setw(9) << "vs ups"
            << "status\n";
        for (const auto& c : results) {
            if (!c.have_entry) {
                out << std::setw(18) << c.kernel << "-- " << c.status
                    << (c.reason.empty() ? "" : ": " + c.reason) << "\n";
                continue;
            }
            const auto& e = c.entry;
            char err_buf[32];
            std::snprintf(err_buf, sizeof(err_buf), "%.2e", e.max_abs_error);
            out << std::setw(18) << c.kernel << std::setw(15) << c.bucket << std::setw(11) << err_buf
                << std::setw(5) << (std::to_string(e.identical_runs) + "/" + std::to_string(e.total_runs))
                << std::setw(10) << fmt(e.wall_ms_median) << std::setw(10)
                << (e.gpu_ms_median ? fmt(*e.gpu_ms_median) : "n/a") << std::setw(10)
                << (e.gflops_gpu() ? fmt(*e.gflops_gpu(), 1) : "n/a") << std::setw(10) << fmt(e.baseline_ms)
                << std::setw(9) << (fmt(e.speedup(), 2) + "x") << std::setw(10)
                << (e.upstream_status == "measured" ? fmt(e.upstream_ms) : "n/a") << std::setw(9)
                << (e.upstream_ratio ? fmt(*e.upstream_ratio, 2) + "x" : "n/a") << c.status << "\n";
            if (!c.reason.empty() && c.status != "PASS (promoted, not default-enabled)")
                out << "      " << c.reason << "\n";
        }
        out << "\nupstream GGML Vulkan op baseline: " << upstream.state
            << "\n('vs ups' = upstream ms / HCS ms: >= 1.05 means the HCS kernel is faster than the upstream "
               "operator)\n"
            << "profile written: " << ppath.string() << "\n";
        print_plans(out, profile);
    }
    if (any_wrong) {
        err << "error: at least one kernel produced wrong results on this device [backend_error]\n";
        return exit_code(ErrorCode::BackendError);
    }
    if (any_error) {
        err << "error: at least one kernel run failed [backend_error]\n";
        return exit_code(ErrorCode::BackendError);
    }
    return 0;
}

}  // namespace hcs::kernels::vulkan
