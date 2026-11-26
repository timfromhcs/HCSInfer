// Real-device tests of the in-process upstream GGML Vulkan baseline (hcs/kernels/ggml_vk_baseline.hpp).
// With no ggml Vulkan device a test reports GTEST_SKIP("... NOT RUN") - never a fake pass.
#include <gtest/gtest.h>

#include <memory>
#include <optional>

#include "hcs/kernels/ggml_vk_baseline.hpp"

using namespace hcs;
namespace gb = hcs::kernels::ggml_baseline;

namespace {

struct Ctx {
    std::optional<gb::Session> session;
    std::string skip_reason;
};

Ctx& ctx() {
    static Ctx* c = [] {
        auto* x = new Ctx();  // leaked on purpose: the backend must outlive static destruction
        auto r = gb::Session::create();
        if (!r)
            x->skip_reason = "no ggml Vulkan device: " + r.error().message;
        else
            x->session.emplace(std::move(r.value()));
        return x;
    }();
    return *c;
}

#define REQUIRE_DEVICE()                                      \
    do {                                                      \
        if (!ctx().session)                                   \
            GTEST_SKIP() << "NOT RUN: " << ctx().skip_reason; \
    } while (0)

constexpr std::uint32_t kSamples = 10;

void expect_sane(const gb::Measurement& m) {
    EXPECT_TRUE(m.check_ok) << "spot check error " << m.max_err_over_l1;
    EXPECT_EQ(m.samples, kSamples);
    ASSERT_EQ(m.raw_ms.size(), kSamples);
    EXPECT_GT(m.median_ms, 0.0);
    EXPECT_LE(m.p10_ms, m.median_ms + 1e-12);
    EXPECT_GE(m.p90_ms, m.median_ms - 1e-12);
    EXPECT_FALSE(m.variant.empty());
}

}  // namespace

TEST(GgmlVkBaseline, UnknownDeviceNameIsNotFoundNeverSilentlyDeviceZero) {
    if (!ctx().session)
        GTEST_SKIP() << "NOT RUN: " << ctx().skip_reason;
    auto r = gb::Session::create("no-such-gpu-name-xyz");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(GgmlVkBaseline, ReportsTheDevice) {
    REQUIRE_DEVICE();
    const auto& d = ctx().session->device();
    EXPECT_GE(d.ggml_index, 0);
    EXPECT_FALSE(d.description.empty());
    EXPECT_FALSE(d.backend.empty());
}

TEST(GgmlVkBaseline, MulMatF32OddShapeIsCorrectAndTimed) {
    REQUIRE_DEVICE();
    auto r = ctx().session->mul_mat(gb::WeightType::F32, 67, 131, 95, kSamples);
    ASSERT_TRUE(r.ok()) << r.error().message;
    expect_sane(r.value());
    EXPECT_DOUBLE_EQ(r.value().flops, 2.0 * 67 * 131 * 95);
}

TEST(GgmlVkBaseline, MulMatF16ReportsTheFasterOfTheActivationVariants) {
    REQUIRE_DEVICE();
    auto r = ctx().session->mul_mat(gb::WeightType::F16, 64, 256, 128, kSamples);
    ASSERT_TRUE(r.ok()) << r.error().message;
    expect_sane(r.value());
    EXPECT_NE(r.value().variant.find("f16 weights"), std::string::npos);
}

TEST(GgmlVkBaseline, QuantisedWeightsRunAgainstTheDequantisedReference) {
    REQUIRE_DEVICE();
    for (auto w : {gb::WeightType::Q8_0, gb::WeightType::Q4_K}) {
        auto r = ctx().session->mul_mat(w, 1, 512, 1024, kSamples);  // decode-shaped GEMV
        ASSERT_TRUE(r.ok()) << gb::to_string(w) << ": " << r.error().message;
        expect_sane(r.value());
        r = ctx().session->mul_mat(w, 32, 512, 1024, kSamples);
        ASSERT_TRUE(r.ok()) << gb::to_string(w) << ": " << r.error().message;
        expect_sane(r.value());
    }
}

TEST(GgmlVkBaseline, RowOperationsAndRopeRun) {
    REQUIRE_DEVICE();
    {
        auto r = ctx().session->softmax(33, 1000, kSamples);
        ASSERT_TRUE(r.ok()) << r.error().message;
        expect_sane(r.value());
    }
    {
        auto r = ctx().session->layernorm(33, 1000, 1e-5f, kSamples);
        ASSERT_TRUE(r.ok()) << r.error().message;
        expect_sane(r.value());
    }
    {
        auto r = ctx().session->rmsnorm(33, 1000, 1e-5f, kSamples);
        ASSERT_TRUE(r.ok()) << r.error().message;
        expect_sane(r.value());
    }
    {
        auto r = ctx().session->rope(48, 128, 0, kSamples);
        ASSERT_TRUE(r.ok()) << r.error().message;
        expect_sane(r.value());
    }
}

TEST(GgmlVkBaseline, InvalidArgumentsAreRejectedBeforeAnyGraphIsBuilt) {
    REQUIRE_DEVICE();
    EXPECT_EQ(ctx().session->mul_mat(gb::WeightType::F32, 0, 8, 8, kSamples).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(ctx().session->mul_mat(gb::WeightType::Q8_0, 4, 8, 33, kSamples).error().code,
              ErrorCode::InvalidArgument);  // k not a multiple of the 32-block
    EXPECT_EQ(ctx().session->mul_mat(gb::WeightType::Q4_K, 4, 8, 128, kSamples).error().code,
              ErrorCode::InvalidArgument);  // k not a multiple of the 256-block
    EXPECT_EQ(ctx().session->softmax(0, 8, kSamples).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(ctx().session->rope(8, 7, 0, kSamples).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(ctx().session->layernorm(8, 0, 1e-5f, kSamples).error().code, ErrorCode::InvalidArgument);
}
