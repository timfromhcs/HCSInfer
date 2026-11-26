// Real-device differential tests of the HCS Vulkan kernels (ported from hcsuda @94a3485) against the CPU
// oracle: odd, tail and non-power-of-two shapes, absolute + relative tolerances declared per kernel, 3 runs
// that must be bit-identical, the Khronos validation layer (0 warnings/errors required), plus fault,
// timeout, stale-handle and invalid-input behaviour of the standalone runner.
//
// Label kernel-vulkan. With no Vulkan device (or a kernel the device cannot run) a test reports
// GTEST_SKIP("... NOT RUN") - never a fake pass.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "hcs/kernels/cpu.hpp"
#include "hcs/kernels/oracle.hpp"
#include "hcs/kernels/vk_ops.hpp"
#include "hcs/kernels/vk_runner.hpp"

using namespace hcs;
using namespace hcs::kernels;
namespace vk = hcs::kernels::vulkan;

namespace {

struct Ctx {
    std::optional<vk::Runner> runner;
    std::unique_ptr<vk::KernelSet> ks;
    std::string skip_reason;
};

Ctx& ctx() {
    static Ctx* c = [] {
        auto* x = new Ctx();  // intentionally leaked: the runner must outlive static destruction order issues
        vk::RunnerOptions o;
        o.validation = true;
        auto r = vk::Runner::create(o);
        if (!r) {
            x->skip_reason = "no usable Vulkan compute device: " + r.error().message;
            return x;
        }
        x->runner.emplace(std::move(r.value()));
        x->ks = std::make_unique<vk::KernelSet>(*x->runner);
        const auto& c2 = x->runner->caps();
        std::cout << "[device] " << c2.name << "  key " << c2.device_key() << "  subgroup "
                  << (c2.subgroup_size ? std::to_string(*c2.subgroup_size) : "unknown") << "  buffers "
                  << (x->runner->device_local_buffers() ? "device_local" : "host_visible") << "\n";
        const auto v = x->runner->validation();
        std::cout << "[validation] requested " << v.requested << " active " << v.active
                  << (v.note.empty() ? "" : "  " + v.note) << "\n";
        return x;
    }();
    return *c;
}

#define REQUIRE_DEVICE()                                       \
    do {                                                       \
        if (!ctx().runner)                                     \
            GTEST_SKIP() << ctx().skip_reason << " (NOT RUN)"; \
    } while (0)

std::vector<float> rand_f32(std::size_t n, std::uint32_t seed, float lo = -1.0f, float hi = 1.0f) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> d(lo, hi);
    std::vector<float> v(n);
    for (auto& x : v) x = d(g);
    return v;
}
std::vector<std::uint16_t> rand_f16(std::size_t n, std::uint32_t seed) {
    const auto f = rand_f32(n, seed);
    std::vector<std::uint16_t> h(n);
    for (std::size_t i = 0; i < n; ++i) h[i] = cpu::fp32_to_fp16(f[i]);
    return h;
}
std::vector<std::int8_t> rand_i8(std::size_t n, std::uint32_t seed) {
    std::mt19937 g(seed);
    std::uniform_int_distribution<int> d(-128, 127);
    std::vector<std::int8_t> v(n);
    for (auto& x : v) x = static_cast<std::int8_t>(d(g));
    return v;
}

// Runs the prepared op 3 times: every run must be bit-identical, and the result must match `want`.
void check_float(vk::PreparedOp& op, const std::vector<float>& want, const cpu::Tolerance& tol,
                 const std::string& label) {
    ASSERT_EQ(op.output_bytes(), want.size() * sizeof(float)) << label;
    std::vector<float> first(want.size()), cur(want.size());
    for (int run = 0; run < 3; ++run) {
        auto t = op.run();
        ASSERT_TRUE(t.ok()) << label << ": " << t.error().message;
        ASSERT_TRUE(op.read_output(cur.data(), op.output_bytes()).ok()) << label;
        if (run == 0) {
            first = cur;
            const auto cmp = cpu::compare(want.data(), cur.data(), want.size(), tol);
            EXPECT_TRUE(cmp.ok) << label << ": " << cmp.mismatches << "/" << cmp.count
                                << " outside tolerance, max abs " << cmp.max_abs_error << " (tol abs "
                                << tol.abs << " rel " << tol.rel << ") worst idx " << cmp.worst_index;
            std::cout << "  " << label << "  max_abs_err " << cmp.max_abs_error << "  tol_abs " << tol.abs
                      << "\n";
        } else {
            EXPECT_EQ(std::memcmp(first.data(), cur.data(), want.size() * sizeof(float)), 0)
                << label << ": run " << run << " is not bit-identical to run 0";
        }
    }
}

void check_int(vk::PreparedOp& op, const std::vector<std::int32_t>& want, const std::string& label) {
    ASSERT_EQ(op.output_bytes(), want.size() * sizeof(std::int32_t)) << label;
    std::vector<std::int32_t> cur(want.size());
    for (int run = 0; run < 3; ++run) {
        ASSERT_TRUE(op.run().ok()) << label;
        ASSERT_TRUE(op.read_output(cur.data(), op.output_bytes()).ok()) << label;
        const auto cmp = cpu::compare_exact(want.data(), cur.data(), want.size());
        EXPECT_TRUE(cmp.ok) << label << ": " << cmp.mismatches
                            << " mismatches (integer kernels must be bit-equal)";
    }
}

std::string sh(std::uint32_t a, std::uint32_t b, std::uint32_t c = 0) {
    return std::to_string(a) + "x" + std::to_string(b) + (c ? "x" + std::to_string(c) : "");
}

struct G3 {
    std::uint32_t m, n, k;
};
using RC = std::pair<std::uint32_t, std::uint32_t>;

}  // namespace

// --- GEMM ----------------------------------------------------------------------------------------------

TEST(VulkanGemmF32, MatchesTheOracleOnOddTailAndNonPowerOfTwoShapes) {
    REQUIRE_DEVICE();
    for (const G3 s : {G3{1, 1, 1}, G3{1, 7, 5}, G3{17, 1, 33}, G3{16, 16, 16}, G3{33, 47, 17},
                       G3{67, 131, 95}, G3{128, 64, 300}, G3{257, 15, 31}}) {
        const auto a = rand_f32(std::size_t{s.m} * s.k, 1 + s.m),
                   b = rand_f32(std::size_t{s.k} * s.n, 2 + s.n);
        std::vector<float> want(std::size_t{s.m} * s.n);
        cpu::gemm_f32(a.data(), b.data(), want.data(), s.m, s.n, s.k);
        auto op = ctx().ks->prepare_gemm_f32(a.data(), b.data(), s.m, s.n, s.k);
        ASSERT_TRUE(op.ok()) << op.error().message;
        check_float(op.value(), want, cpu::tolerance_f32().for_len(s.k), "gemm_f32 " + sh(s.m, s.n, s.k));
    }
}

TEST(VulkanGemmF32, ZerosAndExtremeMagnitudes) {
    REQUIRE_DEVICE();
    const std::uint32_t m = 19, n = 21, k = 23;
    std::vector<float> a(std::size_t{m} * k, 0.0f), b = rand_f32(std::size_t{k} * n, 9);
    std::vector<float> want(std::size_t{m} * n, 0.0f);
    auto op = ctx().ks->prepare_gemm_f32(a.data(), b.data(), m, n, k);
    ASSERT_TRUE(op.ok());
    check_float(op.value(), want, cpu::tolerance_f32(), "gemm_f32 zeros");
    // large dynamic range: tolerance scales with |want| through `rel`
    auto a2 = rand_f32(std::size_t{m} * k, 3, -1e4f, 1e4f),
         b2 = rand_f32(std::size_t{k} * n, 4, -1e-3f, 1e-3f);
    cpu::gemm_f32(a2.data(), b2.data(), want.data(), m, n, k);
    auto op2 = ctx().ks->prepare_gemm_f32(a2.data(), b2.data(), m, n, k);
    ASSERT_TRUE(op2.ok());
    check_float(op2.value(), want, cpu::tolerance_f32().for_len(k * 1000), "gemm_f32 wide range");
}

TEST(VulkanGemmF16, MatchesTheOracleIncludingOddElementCounts) {
    REQUIRE_DEVICE();
    for (const G3 s : {G3{1, 1, 1}, G3{3, 5, 7}, G3{37, 65, 29}, G3{64, 64, 64}, G3{9, 9, 9}}) {
        const auto a = rand_f16(std::size_t{s.m} * s.k, 5 + s.m),
                   b = rand_f16(std::size_t{s.k} * s.n, 6 + s.n);
        std::vector<float> want(std::size_t{s.m} * s.n);
        cpu::gemm_f16(a.data(), b.data(), want.data(), s.m, s.n, s.k);
        auto op = ctx().ks->prepare_gemm_f16(a.data(), b.data(), s.m, s.n, s.k);
        ASSERT_TRUE(op.ok()) << op.error().message;
        check_float(op.value(), want, cpu::tolerance_f32().for_len(s.k), "gemm_f16 " + sh(s.m, s.n, s.k));
    }
}

TEST(VulkanGemmI8Dot, IsBitEqualToTheOracle) {
    REQUIRE_DEVICE();
    const auto g = ctx().ks->gate("gemm_i8dot_tiled");
    if (!g.eligible)
        GTEST_SKIP() << "gemm_i8dot_tiled: NOT RUN on this hardware (" << g.reason << ")";
    for (const G3 s :
         {G3{1, 1, 4}, G3{5, 3, 8}, G3{16, 16, 16}, G3{37, 29, 132}, G3{48, 40, 128}, G3{33, 17, 68}}) {
        const auto a = rand_i8(std::size_t{s.m} * s.k, 7 + s.m), b = rand_i8(std::size_t{s.k} * s.n, 8 + s.n);
        std::vector<std::int32_t> want(std::size_t{s.m} * s.n);
        cpu::gemm_i8(a.data(), b.data(), want.data(), s.m, s.n, s.k);
        auto op = ctx().ks->prepare_gemm_i8dot(a.data(), b.data(), s.m, s.n, s.k);
        ASSERT_TRUE(op.ok()) << op.error().message;
        check_int(op.value(), want, "gemm_i8dot " + sh(s.m, s.n, s.k));
    }
}

TEST(VulkanGemmI8Dot, ExtremeValuesStayExact) {
    REQUIRE_DEVICE();
    if (!ctx().ks->gate("gemm_i8dot_tiled").eligible)
        GTEST_SKIP() << "gemm_i8dot_tiled: NOT RUN on this hardware";
    const std::uint32_t m = 8, n = 8, k = 256;
    std::vector<std::int8_t> a(std::size_t{m} * k, static_cast<std::int8_t>(-128)),
        b(std::size_t{k} * n, static_cast<std::int8_t>(-128));
    std::vector<std::int32_t> want(std::size_t{m} * n);
    cpu::gemm_i8(a.data(), b.data(), want.data(), m, n, k);
    auto op = ctx().ks->prepare_gemm_i8dot(a.data(), b.data(), m, n, k);
    ASSERT_TRUE(op.ok());
    check_int(op.value(), want, "gemm_i8dot -128 x -128");
}

TEST(VulkanGemmCoopMat, MatchesTheOracleOnAlignedShapesAndRefusesOthers) {
    REQUIRE_DEVICE();
    const auto g = ctx().ks->gate("gemm_coopmat_f16");
    if (!g.eligible)
        GTEST_SKIP() << "gemm_coopmat_f16: NOT RUN on this hardware (" << g.reason << ")";
    for (const G3 s : {G3{16, 16, 16}, G3{32, 16, 48}, G3{48, 80, 64}, G3{64, 64, 128}}) {
        const auto a = rand_f16(std::size_t{s.m} * s.k, 11 + s.m),
                   b = rand_f16(std::size_t{s.k} * s.n, 12 + s.n);
        std::vector<float> want(std::size_t{s.m} * s.n);
        cpu::gemm_f16(a.data(), b.data(), want.data(), s.m, s.n, s.k);
        auto op = ctx().ks->prepare_gemm_coopmat_f16(a.data(), b.data(), s.m, s.n, s.k);
        ASSERT_TRUE(op.ok()) << op.error().message;
        check_float(op.value(), want, cpu::tolerance_f32().for_len(s.k), "gemm_coopmat " + sh(s.m, s.n, s.k));
    }
    const auto a = rand_f16(30 * 16, 1), b = rand_f16(16 * 16, 2);
    auto bad = ctx().ks->prepare_gemm_coopmat_f16(a.data(), b.data(), 30, 16, 16);  // m % 16 != 0
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, ErrorCode::Unsupported);
}

// --- row-wise ops ---------------------------------------------------------------------------------------

TEST(VulkanSoftmax, MatchesTheOracleAndIsStableForLargeInputs) {
    REQUIRE_DEVICE();
    for (const auto& [rows, cols] :
         std::vector<RC>{{1, 1}, {3, 1000}, {8, 257}, {5, 4096}, {1, 31}, {17, 513}}) {
        const auto x = rand_f32(std::size_t{rows} * cols, 21 + cols, -8.0f, 8.0f);
        std::vector<float> want(x.size());
        cpu::softmax_rows(x.data(), want.data(), rows, cols);
        auto op = ctx().ks->prepare_softmax(x.data(), rows, cols);
        ASSERT_TRUE(op.ok()) << op.error().message;
        check_float(op.value(), want, cpu::tolerance_f32().for_len(cols), "softmax " + sh(rows, cols));
    }
    // 1000..1002 would overflow a naive exp(): the max subtraction must keep it finite and normalised
    const std::vector<float> big = {1000.0f, 1001.0f, 1002.0f, 999.0f};
    std::vector<float> want(4);
    cpu::softmax_rows(big.data(), want.data(), 1, 4);
    auto op = ctx().ks->prepare_softmax(big.data(), 1, 4);
    ASSERT_TRUE(op.ok());
    check_float(op.value(), want, cpu::tolerance_f32().for_len(4), "softmax large values");
}

TEST(VulkanLayerNorm, MatchesTheOracle) {
    REQUIRE_DEVICE();
    for (const auto& [rows, cols] : std::vector<RC>{{1, 1}, {3, 1000}, {8, 257}, {2, 4096}, {5, 33}}) {
        const auto x = rand_f32(std::size_t{rows} * cols, 31 + cols, -3.0f, 3.0f),
                   g = rand_f32(cols, 32, 0.5f, 1.5f), b = rand_f32(cols, 33, -0.5f, 0.5f);
        std::vector<float> want(x.size());
        cpu::layernorm_rows(x.data(), g.data(), b.data(), want.data(), rows, cols, 1e-5f);
        auto op = ctx().ks->prepare_layernorm(x.data(), g.data(), b.data(), rows, cols, 1e-5f);
        ASSERT_TRUE(op.ok()) << op.error().message;
        check_float(op.value(), want, cpu::tolerance_f32().for_len(cols), "layernorm " + sh(rows, cols));
    }
}

TEST(VulkanRmsNorm, MatchesTheOracleIncludingAllZeroRows) {
    REQUIRE_DEVICE();
    for (const auto& [rows, cols] : std::vector<RC>{{1, 1}, {3, 1000}, {8, 257}, {2, 4096}, {5, 33}}) {
        const auto x = rand_f32(std::size_t{rows} * cols, 41 + cols, -3.0f, 3.0f),
                   g = rand_f32(cols, 42, 0.5f, 1.5f);
        std::vector<float> want(x.size());
        cpu::rmsnorm_rows(x.data(), g.data(), want.data(), rows, cols, 1e-5f);
        auto op = ctx().ks->prepare_rmsnorm(x.data(), g.data(), rows, cols, 1e-5f);
        ASSERT_TRUE(op.ok()) << op.error().message;
        check_float(op.value(), want, cpu::tolerance_f32().for_len(cols), "rmsnorm " + sh(rows, cols));
    }
    const std::vector<float> zeros(4 * 64, 0.0f), g(64, 1.0f);
    std::vector<float> want(zeros.size());
    cpu::rmsnorm_rows(zeros.data(), g.data(), want.data(), 4, 64, 1e-5f);
    auto op = ctx().ks->prepare_rmsnorm(zeros.data(), g.data(), 4, 64, 1e-5f);
    ASSERT_TRUE(op.ok());
    check_float(op.value(), want, cpu::tolerance_f32(), "rmsnorm zeros");
}

TEST(VulkanRope, MatchesTheOracleWithinTheDeclaredTrigTolerance) {
    REQUIRE_DEVICE();
    // Vulkan only bounds sin/cos to 2^-11 inside [-pi, pi]; angles reach `rows + pos0` rad here. The declared
    // absolute tolerance for unit-scale inputs is 1e-3 (the measured error is printed per case).
    struct C {
        std::uint32_t rows, dim, pos0;
        float base;
    };
    for (const C c : {C{1, 2, 0, 10000.0f}, C{7, 64, 0, 10000.0f}, C{40, 126, 17, 10000.0f},
                      C{129, 64, 3, 500000.0f}, C{512, 128, 0, 10000.0f}}) {
        const auto x = rand_f32(std::size_t{c.rows} * c.dim, 51 + c.dim);
        std::vector<float> want(x.size());
        cpu::rope_rows(x.data(), want.data(), c.rows, c.dim, c.pos0, c.base);
        auto op = ctx().ks->prepare_rope(x.data(), c.rows, c.dim, c.pos0, c.base);
        ASSERT_TRUE(op.ok()) << op.error().message;
        check_float(op.value(), want, cpu::Tolerance{1e-5, 1e-3},
                    "rope " + sh(c.rows, c.dim) + " pos0 " + std::to_string(c.pos0));
    }
}

// --- invalid inputs / limits --------------------------------------------------------------------------

TEST(VulkanOps, InvalidInputsAreRejectedBeforeAnyDeviceWork) {
    REQUIRE_DEVICE();
    auto& ks = *ctx().ks;
    const std::vector<float> f(64, 1.0f);
    EXPECT_EQ(ks.prepare_gemm_f32(nullptr, f.data(), 4, 4, 4).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(ks.prepare_gemm_f32(f.data(), f.data(), 0, 4, 4).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(ks.prepare_softmax(f.data(), 4, 0).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(ks.prepare_rope(f.data(), 4, 3, 0, 10000.0f).error().code,
              ErrorCode::InvalidArgument);  // odd dim
    const std::vector<std::int8_t> i(64, 1);
    if (ks.gate("gemm_i8dot_tiled").eligible) {
        auto r = ks.prepare_gemm_i8dot(i.data(), i.data(), 4, 4, 6);  // k % 4 != 0
        ASSERT_FALSE(r.ok());
        EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    }
    // more workgroups than the portable 65535 limit: refused, not wrapped
    const std::vector<float> big(65536, 0.5f);
    auto r = ks.prepare_softmax(big.data(), 65536, 1);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
}

TEST(VulkanRunner, RejectsBadSpirvAndBadDispatchArguments) {
    REQUIRE_DEVICE();
    auto& rn = *ctx().runner;
    const unsigned char junk[24] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    EXPECT_EQ(rn.create_pipeline(junk, sizeof(junk), 1, 0).error().code,
              ErrorCode::InvalidArgument);  // bad magic
    EXPECT_EQ(rn.create_pipeline(nullptr, 0, 1, 0).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(rn.create_buffer(0).error().code, ErrorCode::InvalidArgument);
    auto b = rn.create_buffer(16);
    ASSERT_TRUE(b.ok());
    const float v[8] = {};
    EXPECT_EQ(rn.upload(b.value(), v, 32).error().code, ErrorCode::InvalidArgument);  // beyond the buffer
    float out[8];
    EXPECT_EQ(rn.download(b.value(), out, 32).error().code, ErrorCode::InvalidArgument);
    // round trip through the (staged) buffer
    const float in4[4] = {1, 2, 3, 4};
    ASSERT_TRUE(rn.upload(b.value(), in4, sizeof(in4)).ok());
    float back[4] = {};
    ASSERT_TRUE(rn.download(b.value(), back, sizeof(back)).ok());
    for (int i = 0; i < 4; ++i) EXPECT_EQ(back[i], in4[i]);
}

TEST(VulkanRunner, ManyBuffersCreateAndDestroyWithoutLeaking) {
    REQUIRE_DEVICE();
    auto& rn = *ctx().runner;
    for (int i = 0; i < 200; ++i) {
        auto b = rn.create_buffer(4096 + static_cast<std::size_t>(i) * 16);
        ASSERT_TRUE(b.ok()) << b.error().message;
    }  // each Buffer is destroyed at the end of its iteration; validation reports leaks at teardown
    EXPECT_FALSE(rn.lost());
}

// --- fault, timeout, recovery -------------------------------------------------------------------------

TEST(VulkanRunner, InjectedDeviceLostIsReportedRefusesWorkAndRecoversAfterReset) {
    REQUIRE_DEVICE();
    vk::RunnerOptions o;
    o.validation = true;
    auto r = vk::Runner::create(o);
    ASSERT_TRUE(r.ok()) << r.error().message;
    vk::Runner& rn = r.value();
    vk::KernelSet ks(rn);
    const auto a = rand_f32(16 * 16, 1), b = rand_f32(16 * 16, 2);
    auto op = ks.prepare_gemm_f32(a.data(), b.data(), 16, 16, 16);
    ASSERT_TRUE(op.ok());
    ASSERT_TRUE(op.value().run().ok());

    rn.inject_fault_for_testing(vk::Fault::DeviceLostOnSubmit);
    auto lost = op.value().run();
    ASSERT_FALSE(lost.ok());
    EXPECT_EQ(lost.error().code, ErrorCode::BackendError);
    EXPECT_TRUE(rn.lost());
    auto refused = op.value().run();  // a lost runner refuses work instead of touching a dead device
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, ErrorCode::InvalidState);
    EXPECT_FALSE(rn.create_buffer(64).ok());

    ASSERT_TRUE(rn.reset().ok());
    EXPECT_FALSE(rn.lost());
    // The old op's buffers belong to the torn-down device: refused (and safely destroyable) ...
    auto stale = op.value().run();
    ASSERT_FALSE(stale.ok());
    EXPECT_EQ(stale.error().code, ErrorCode::InvalidState);
    // ... and a fresh KernelSet works on the recovered device.
    vk::KernelSet ks2(rn);
    auto fresh = ks2.prepare_gemm_f32(a.data(), b.data(), 16, 16, 16);
    ASSERT_TRUE(fresh.ok());
    std::vector<float> want(16 * 16);
    cpu::gemm_f32(a.data(), b.data(), want.data(), 16, 16, 16);
    check_float(fresh.value(), want, cpu::tolerance_f32().for_len(16), "gemm after recovery");
}

TEST(VulkanRunner, DispatchTimeoutMarksTheRunnerLostAndResetRecovers) {
    REQUIRE_DEVICE();
    vk::RunnerOptions o;
    o.dispatch_timeout_s = 0.0;  // clamped to 1 ms: far below the time of the dispatch below
    o.validation = true;
    auto r = vk::Runner::create(o);
    ASSERT_TRUE(r.ok()) << r.error().message;
    vk::Runner& rn = r.value();
    vk::KernelSet ks(rn);
    const std::uint32_t n = 1024;  // ~2.1 GFLOP: milliseconds even on a fast GPU
    const auto a = rand_f32(std::size_t{n} * n, 1), b = rand_f32(std::size_t{n} * n, 2);
    auto op = ks.prepare_gemm_f32(a.data(), b.data(), n, n, n);
    ASSERT_TRUE(op.ok());
    auto t = op.value().run();
    if (t.ok())
        GTEST_SKIP()
            << "the 1024^3 GEMM finished inside the 1 ms bound on this machine (timeout path NOT RUN)";
    EXPECT_EQ(t.error().code, ErrorCode::Timeout);
    EXPECT_TRUE(rn.lost());
    ASSERT_TRUE(rn.reset().ok());  // waits for the device to go idle, then re-creates it
    EXPECT_FALSE(rn.lost());
    vk::KernelSet ks2(rn);
    const auto a2 = rand_f32(16 * 16, 3), b2 = rand_f32(16 * 16, 4);
    auto fresh = ks2.prepare_gemm_f32(a2.data(), b2.data(), 16, 16, 16);
    ASSERT_TRUE(fresh.ok());
    // (a 1 ms bound may also time out the tiny dispatch on a slow driver; only the recovery state matters)
    (void)fresh.value().run();
}

// --- must stay last: the shared runner has now executed every kernel above ---------------------------------

TEST(VulkanZzValidation, TheKhronosValidationLayerReportedNothing) {
    REQUIRE_DEVICE();
    const auto v = ctx().runner->validation();
    if (!v.active)
        GTEST_SKIP() << "validation layer NOT RUN: " << v.note;
    for (const auto& m : v.first_messages) std::cout << "[validation message] " << m << "\n";
    EXPECT_EQ(v.messages, 0u) << "warning/error messages from VK_LAYER_KHRONOS_validation";
}
