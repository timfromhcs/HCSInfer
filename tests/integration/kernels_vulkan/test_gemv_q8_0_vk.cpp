// Real-device correctness of the HCS Vulkan Q8_0 GEMV kernel against the CPU reference (AGENTS.md M8).
// The kernel is feature-flagged: these tests enable it explicitly. When no Vulkan compute device is
// reachable the tests SKIP (NOT RUN), never a fake pass.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "hcs/kernels/cpu.hpp"
#include "hcs/kernels/vulkan.hpp"
#include "q8_0_data.hpp"

using namespace hcs;
using namespace hcs::kernels;

namespace {

void require_device() {
    if (!vulkan::available())
        GTEST_SKIP() << "no Vulkan compute device (NOT RUN)";
}

}  // namespace

TEST(KernelVulkanGemv, FeatureFlagIsOffByDefault) {
    vulkan::set_enabled(false);
    const std::uint32_t m = 4, k = 32;
    std::vector<float> x(k, 1.0f), y(m, 0.0f);
    const auto w = test::to_q8_0(std::vector<float>(m * k, 0.5f), m, k);
    const vulkan::GemvRequest req{x.data(), w.data(), y.data(), m, k};
    auto r = vulkan::gemv_q8_0(req);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
}

TEST(KernelVulkanGemv, ShapeValidation) {
    vulkan::set_enabled(true);
    std::vector<float> x(32, 1.0f), y(1, 0.0f);
    const auto w = test::to_q8_0(std::vector<float>(32, 0.5f), 1, 32);
    EXPECT_EQ(vulkan::gemv_q8_0(vulkan::GemvRequest{x.data(), w.data(), y.data(), 1, 31}).error().code,
              ErrorCode::InvalidArgument);  // k not a multiple of 32
    EXPECT_EQ(vulkan::gemv_q8_0(vulkan::GemvRequest{x.data(), w.data(), y.data(), 0, 32}).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(vulkan::gemv_q8_0(vulkan::GemvRequest{nullptr, w.data(), y.data(), 1, 32}).error().code,
              ErrorCode::InvalidArgument);
}

TEST(KernelVulkanGemv, MatchesTheCpuReferenceAcrossShapes) {
    require_device();
    vulkan::set_enabled(true);
    auto info = vulkan::device_info();
    ASSERT_TRUE(info.ok()) << info.error().message;
    std::cout << "[device] " << info.value().name << " (" << info.value().device_type << ")\n";

    struct Shape {
        std::uint32_t m, k;
    };
    for (const auto variant : {vulkan::Variant::Tiled, vulkan::Variant::Scalar}) {
        for (const Shape s : {Shape{1, 32}, Shape{7, 160}, Shape{64, 256}, Shape{129, 512}, Shape{512, 1024},
                              Shape{1024, 2048}}) {
            const auto wf = test::random_vec(static_cast<std::size_t>(s.m) * s.k, 100 + s.m + s.k);
            const auto w = test::to_q8_0(wf, s.m, s.k);
            const auto x = test::random_vec(s.k, 200 + s.m + s.k);
            std::vector<float> y_cpu(s.m, 0.0f), y_vk(s.m, 0.0f);
            cpu::gemv_q8_0(x.data(), w.data(), y_cpu.data(), s.m, s.k);
            auto r = vulkan::gemv_q8_0(vulkan::GemvRequest{x.data(), w.data(), y_vk.data(), s.m, s.k}, {},
                                       variant);
            ASSERT_TRUE(r.ok()) << vulkan::to_string(variant) << " m=" << s.m << " k=" << s.k << ": "
                                << r.error().message;
            for (std::uint32_t i = 0; i < s.m; ++i) {
                const float tol = 1e-3f * (1.0f + std::fabs(y_cpu[i]));
                EXPECT_NEAR(y_vk[i], y_cpu[i], tol)
                    << vulkan::to_string(variant) << " row " << i << " m=" << s.m << " k=" << s.k;
            }
        }
    }
}

TEST(KernelVulkanGemv, RepeatsAreStableAndTheDeviceSurvivesManyCalls) {
    require_device();
    vulkan::set_enabled(true);
    const std::uint32_t m = 96, k = 320;
    const auto wf = test::random_vec(static_cast<std::size_t>(m) * k, 5);
    const auto w = test::to_q8_0(wf, m, k);
    const auto x = test::random_vec(k, 6);
    std::vector<float> first(m, 0.0f);
    ASSERT_TRUE(vulkan::gemv_q8_0(vulkan::GemvRequest{x.data(), w.data(), first.data(), m, k}).ok());
    for (int i = 0; i < 20; ++i) {
        std::vector<float> y(m, 0.0f);
        ASSERT_TRUE(vulkan::gemv_q8_0(vulkan::GemvRequest{x.data(), w.data(), y.data(), m, k}).ok()) << i;
        EXPECT_EQ(y, first) << "call " << i << " differs from the first (must be deterministic)";
    }
}

namespace {

// The caller-side contract documented in docs/vulkan-kernels.md: try the GPU, keep the CPU result on any
// error.
std::vector<float> gemv_with_cpu_fallback(const std::vector<float>& x, const std::vector<std::uint8_t>& w,
                                          std::uint32_t m, std::uint32_t k, bool* used_gpu) {
    std::vector<float> y(m, 0.0f);
    auto r = vulkan::gemv_q8_0(vulkan::GemvRequest{x.data(), w.data(), y.data(), m, k});
    *used_gpu = r.ok();
    if (!r.ok())
        cpu::gemv_q8_0(x.data(), w.data(), y.data(), m, k);
    return y;
}

}  // namespace

// Works without a GPU: with the flag off the documented fallback pattern must still give the right answer.
TEST(KernelVulkanFallback, DisabledFlagFallsBackToTheCpuReferenceWithTheCorrectResult) {
    vulkan::set_enabled(false);
    const std::uint32_t m = 40, k = 96;
    const auto w = test::to_q8_0(test::random_vec(static_cast<std::size_t>(m) * k, 11), m, k);
    const auto x = test::random_vec(k, 12);
    std::vector<float> ref(m, 0.0f);
    cpu::gemv_q8_0(x.data(), w.data(), ref.data(), m, k);
    bool used_gpu = true;
    const auto y = gemv_with_cpu_fallback(x, w, m, k, &used_gpu);
    EXPECT_FALSE(used_gpu);
    EXPECT_EQ(y, ref);
}

// AGENTS.md 12.4: on VK_ERROR_DEVICE_LOST stop submitting, report a BackendError, and recover only through a
// controlled path (the cached context is rebuilt by the next call). The loss is injected at the submit seam;
// the real queue is not touched, so the test is safe on the real device.
TEST(KernelVulkanFallback, InjectedDeviceLossIsReportedFallsBackToCpuAndTheNextCallRecovers) {
    require_device();
    vulkan::set_enabled(true);
    const std::uint32_t m = 64, k = 256;
    const auto w = test::to_q8_0(test::random_vec(static_cast<std::size_t>(m) * k, 21), m, k);
    const auto x = test::random_vec(k, 22);
    std::vector<float> ref(m, 0.0f);
    cpu::gemv_q8_0(x.data(), w.data(), ref.data(), m, k);

    vulkan::inject_fault_for_testing(vulkan::TestFault::DeviceLostOnSubmit);
    std::vector<float> y(m, 0.0f);
    auto r = vulkan::gemv_q8_0(vulkan::GemvRequest{x.data(), w.data(), y.data(), m, k});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::BackendError);
    EXPECT_NE(r.error().message.find("device lost"), std::string::npos) << r.error().message;

    // The caller keeps a correct CPU result.
    vulkan::inject_fault_for_testing(vulkan::TestFault::DeviceLostOnSubmit);
    bool used_gpu = true;
    const auto y_fb = gemv_with_cpu_fallback(x, w, m, k, &used_gpu);
    EXPECT_FALSE(used_gpu);
    EXPECT_EQ(y_fb, ref);

    // Fault consumed: the next call rebuilds the context and matches the reference again.
    std::vector<float> y2(m, 0.0f);
    ASSERT_TRUE(vulkan::gemv_q8_0(vulkan::GemvRequest{x.data(), w.data(), y2.data(), m, k}).ok());
    for (std::uint32_t i = 0; i < m; ++i) EXPECT_NEAR(y2[i], ref[i], 1e-3f * (1.0f + std::fabs(ref[i]))) << i;
    vulkan::inject_fault_for_testing(vulkan::TestFault::None);
}
