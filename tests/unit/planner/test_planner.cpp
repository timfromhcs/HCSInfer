// Conservative planner + promotion rule. Cases ported from hcsuda crates/hcsuda-core/src/planner.rs tests
// ("fresh device falls back", "unverified path is not used", "coopmat only for aligned shapes", ...),
// extended for HCSInfer's evidence model (profile entries keyed by device/driver/shader hash/bucket).
#include <gtest/gtest.h>

#include "planner_fixtures.hpp"

using namespace hcs::test;

namespace {
const Shape kGemm{256, 1024, 256};

KernelHashes hashes_for_all() {
    KernelHashes h;
    for (const auto& k : kernel_catalog()) h[k.id] = std::string("hash_") + k.id;
    return h;
}

PlanPolicy opt_in() {
    PlanPolicy p;
    p.allow_unbaselined = true;
    return p;
}
}  // namespace

TEST(Planner, FreshDeviceFallsBackToReference) {
    const DeviceCaps c = caps_680m();
    const Plan p = plan(Op::Gemm, DType::F32, kGemm, c, KernelProfile{}, hashes_for_all());
    EXPECT_EQ(p.kind, PathKind::Reference);
    EXPECT_TRUE(p.kernel_id.empty());
    EXPECT_NE(p.reason.find("no verified"), std::string::npos);
}

TEST(Planner, UnknownKernelHashMeansCpu) {
    const DeviceCaps c = caps_680m();
    KernelProfile prof;
    ASSERT_TRUE(prof.upsert(good_entry(c, "gemm_f32_tiled", "hash_gemm_f32_tiled", kGemm)).ok());
    const Plan p = plan(Op::Gemm, DType::F32, kGemm, c, prof, KernelHashes{}, opt_in());
    EXPECT_EQ(p.kind, PathKind::Reference);
    EXPECT_NE(p.reason.find("hash unknown"), std::string::npos);
}

TEST(Planner, UnknownCapabilitiesNeverReachTheGpuEvenWithEvidence) {
    const DeviceCaps c = caps_from_probe(old_vulkan_device_probe());
    KernelProfile prof;
    ASSERT_TRUE(prof.upsert(good_entry(c, "gemm_f32_tiled", "hash_gemm_f32_tiled", kGemm)).ok());
    const Plan p = plan(Op::Gemm, DType::F32, kGemm, c, prof, hashes_for_all(), opt_in());
    EXPECT_EQ(p.kind, PathKind::Reference);  // shared memory / invocations / push constants are unknown
}

TEST(Planner, PromotedKernelWithoutMeasuredUpstreamBaselineIsOnlyACandidate) {
    const DeviceCaps c = caps_680m();
    KernelProfile prof;
    ASSERT_TRUE(prof.upsert(good_entry(c, "gemm_f32_tiled", "hash_gemm_f32_tiled", kGemm)).ok());
    const Plan p = plan(Op::Gemm, DType::F32, kGemm, c, prof, hashes_for_all());
    EXPECT_EQ(p.kind, PathKind::Reference)
        << "release default must not enable a kernel without an upstream baseline";
    EXPECT_EQ(p.candidate_kernel_id, "gemm_f32_tiled");
    EXPECT_EQ(p.candidate_status, "promoted");
    EXPECT_NE(p.reason.find("NOT RUN"), std::string::npos);
}

TEST(Planner, UpstreamBaselineAtOrAboveThresholdEnablesByDefault) {
    const DeviceCaps c = caps_680m();
    ProfileEntry e = good_entry(c, "gemm_f32_tiled", "hash_gemm_f32_tiled", kGemm);
    e.upstream_status = "measured";
    e.upstream_ratio = 1.05;
    KernelProfile prof;
    ASSERT_TRUE(prof.upsert(e).ok());
    const Plan p = plan(Op::Gemm, DType::F32, kGemm, c, prof, hashes_for_all());
    EXPECT_EQ(p.kind, PathKind::VulkanTiled);
    EXPECT_EQ(p.kernel_id, "gemm_f32_tiled");

    ProfileEntry slow = e;
    slow.upstream_ratio = 1.04;  // measured, but below the rule
    KernelProfile prof2;
    ASSERT_TRUE(prof2.upsert(slow).ok());
    EXPECT_EQ(plan(Op::Gemm, DType::F32, kGemm, c, prof2, hashes_for_all()).kind, PathKind::Reference);
}

TEST(Planner, OptInSeamUsesPromotedKernelsButStillRequiresPromotion) {
    const DeviceCaps c = caps_680m();
    KernelProfile prof;
    ASSERT_TRUE(prof.upsert(good_entry(c, "gemm_f32_tiled", "hash_gemm_f32_tiled", kGemm)).ok());
    EXPECT_EQ(plan(Op::Gemm, DType::F32, kGemm, c, prof, hashes_for_all(), opt_in()).kind,
              PathKind::VulkanTiled);

    ProfileEntry slow = good_entry(c, "gemm_f32_tiled", "hash_gemm_f32_tiled", kGemm);
    slow.baseline_ms = 1.04;  // 1.04x
    KernelProfile prof2;
    ASSERT_TRUE(prof2.upsert(slow).ok());
    const Plan p = plan(Op::Gemm, DType::F32, kGemm, c, prof2, hashes_for_all(), opt_in());
    EXPECT_EQ(p.kind, PathKind::Reference);
    EXPECT_EQ(p.candidate_status, "rejected");
}

TEST(Planner, EvidenceForAnotherDriverShaderOrBucketIsIgnored) {
    const DeviceCaps c = caps_680m();
    KernelProfile prof;
    ASSERT_TRUE(prof.upsert(good_entry(c, "gemm_f32_tiled", "hash_gemm_f32_tiled", kGemm)).ok());

    DeviceCaps newer_driver = c;
    newer_driver.driver_version_raw = "0x8008B000";
    Plan p = plan(Op::Gemm, DType::F32, kGemm, newer_driver, prof, hashes_for_all(), opt_in());
    EXPECT_EQ(p.kind, PathKind::Reference);
    EXPECT_EQ(p.candidate_status, "") << "stale evidence must not even be a candidate";
    EXPECT_NE(p.reason.find("stale"), std::string::npos);

    KernelHashes edited = hashes_for_all();
    edited["gemm_f32_tiled"] = "edited_shader";
    EXPECT_EQ(plan(Op::Gemm, DType::F32, kGemm, c, prof, edited, opt_in()).kind, PathKind::Reference);

    const Shape other_bucket{4096, 4096, 4096};
    EXPECT_EQ(plan(Op::Gemm, DType::F32, other_bucket, c, prof, hashes_for_all(), opt_in()).kind,
              PathKind::Reference);
    // same bucket, different exact shape: evidence applies (pow2 buckets)
    const Shape same_bucket{200, 900, 130};
    EXPECT_EQ(shape_bucket(same_bucket), shape_bucket(kGemm));
    EXPECT_EQ(plan(Op::Gemm, DType::F32, same_bucket, c, prof, hashes_for_all(), opt_in()).kind,
              PathKind::VulkanTiled);
}

TEST(Planner, CoopMatIsPreferredForAlignedF16AndTiledForRaggedShapes) {
    const DeviceCaps c = caps_680m();
    const Shape aligned{64, 32, 48}, ragged{64, 30, 48};
    KernelProfile prof;
    for (const Shape& s : {aligned, ragged}) {
        ASSERT_TRUE(prof.upsert(good_entry(c, "gemm_coopmat_f16", "hash_gemm_coopmat_f16", s)).ok());
        ASSERT_TRUE(prof.upsert(good_entry(c, "gemm_f16_tiled", "hash_gemm_f16_tiled", s)).ok());
    }
    const KernelHashes h = hashes_for_all();
    EXPECT_EQ(plan(Op::Gemm, DType::F16, aligned, c, prof, h, opt_in()).kind, PathKind::VulkanCoopMat);
    EXPECT_EQ(plan(Op::Gemm, DType::F16, ragged, c, prof, h, opt_in()).kind, PathKind::VulkanTiled);
    // f32 never picks the f16 coopmat kernel
    EXPECT_EQ(plan(Op::Gemm, DType::F32, aligned, c, prof, h, opt_in()).kind, PathKind::Reference);
}

TEST(Planner, Int8UsesDotKernelOnlyWhenVerifiedAndSupported) {
    const DeviceCaps c = caps_680m();
    const Shape s{64, 64, 64};
    EXPECT_EQ(plan(Op::Gemm, DType::I8, s, c, KernelProfile{}, hashes_for_all(), opt_in()).kind,
              PathKind::Reference);
    KernelProfile prof;
    ASSERT_TRUE(prof.upsert(good_entry(c, "gemm_i8dot_tiled", "hash_gemm_i8dot_tiled", s)).ok());
    EXPECT_EQ(plan(Op::Gemm, DType::I8, s, c, prof, hashes_for_all(), opt_in()).kind, PathKind::VulkanIntDot);
    DeviceCaps nodot = c;
    nodot.integer_dot_product = Cap::No;
    const Plan p = plan(Op::Gemm, DType::I8, s, nodot, prof, hashes_for_all(), opt_in());
    EXPECT_EQ(p.kind, PathKind::Reference);
    EXPECT_NE(p.reason.find("no compute path"), std::string::npos) << p.reason;
}

TEST(Planner, RowOpsAndRopeFollowTheSameRule) {
    const DeviceCaps c = caps_680m();
    const Shape rows{32, 4096, 0}, rope{512, 128, 0};
    KernelProfile prof;
    ASSERT_TRUE(prof.upsert(good_entry(c, "softmax_rows", "hash_softmax_rows", rows)).ok());
    ASSERT_TRUE(prof.upsert(good_entry(c, "rope_rows", "hash_rope_rows", rope)).ok());
    const KernelHashes h = hashes_for_all();
    EXPECT_EQ(plan(Op::Softmax, DType::F32, rows, c, prof, h, opt_in()).kind, PathKind::VulkanTiled);
    EXPECT_EQ(plan(Op::Rope, DType::F32, rope, c, prof, h, opt_in()).kind, PathKind::VulkanScalar);
    EXPECT_EQ(plan(Op::LayerNorm, DType::F32, rows, c, prof, h, opt_in()).kind, PathKind::Reference);
    const Shape odd_dim{512, 127, 0};
    EXPECT_EQ(plan(Op::Rope, DType::F32, odd_dim, c, prof, h, opt_in()).kind, PathKind::Reference);
}

TEST(Planner, OpsWithoutAKernelStayOnTheCpu) {
    const DeviceCaps c = caps_680m();
    const Plan p = plan(Op::Softmax, DType::F16, {8, 8, 0}, c, KernelProfile{}, hashes_for_all(), opt_in());
    EXPECT_EQ(p.kind, PathKind::Reference);
    EXPECT_NE(p.reason.find("no HCS Vulkan kernel"), std::string::npos);
}

// --- promotion rule ----------------------------------------------------------------------------------

TEST(Promotion, RequiresCorrectnessDeterminismQuietTimingAndSpeedup) {
    const DeviceCaps c = caps_680m();
    const ProfileEntry good = good_entry(c, "gemm_f32_tiled", "h", kGemm);
    EXPECT_TRUE(evaluate_promotion(good).promoted);

    ProfileEntry wrong = good;
    wrong.correct = false;
    EXPECT_FALSE(evaluate_promotion(wrong).promoted);
    EXPECT_FALSE(evaluate_promotion(wrong).correct_ok);

    ProfileEntry two_runs = good;
    two_runs.total_runs = 2;
    two_runs.identical_runs = 2;
    EXPECT_FALSE(evaluate_promotion(two_runs).determinism_ok) << "needs 3 runs, 2 identical is not enough";

    ProfileEntry flaky = good;
    flaky.identical_runs = 2;  // 2 of 3 bit-identical
    EXPECT_FALSE(evaluate_promotion(flaky).promoted);

    ProfileEntry noisy = good;
    noisy.wall_spread = 0.26;
    EXPECT_FALSE(evaluate_promotion(noisy).noise_ok);
    noisy.wall_spread = 0.25;
    EXPECT_TRUE(evaluate_promotion(noisy).noise_ok);

    ProfileEntry nobase = good;
    nobase.baseline_ms = 0.0;
    EXPECT_FALSE(evaluate_promotion(nobase).speed_ok);

    ProfileEntry nan_err = good;
    nan_err.max_abs_error = std::nan("");
    EXPECT_FALSE(evaluate_promotion(nan_err).correct_ok);
}

TEST(Promotion, SpeedupThresholdIsOnePointZeroFive) {
    const DeviceCaps c = caps_680m();
    ProfileEntry e = good_entry(c, "gemm_f32_tiled", "h", kGemm);
    e.wall_ms_median = 1.0;
    e.baseline_ms = 1.049;
    EXPECT_FALSE(evaluate_promotion(e).speed_ok);
    e.baseline_ms = 1.05;
    EXPECT_TRUE(evaluate_promotion(e).speed_ok);
    e.baseline_ms = 1.2;
    EXPECT_TRUE(evaluate_promotion(e).speed_ok);
    EXPECT_NEAR(e.speedup(), 1.2, 1e-12);
    // custom policy
    PromotionPolicy strict;
    strict.min_speedup = 1.5;
    EXPECT_FALSE(evaluate_promotion(e, strict).speed_ok);
}

TEST(Promotion, DefaultEnableNeedsAMeasuredUpstreamBaseline) {
    const DeviceCaps c = caps_680m();
    ProfileEntry e = good_entry(c, "gemm_f32_tiled", "h", kGemm);
    PromotionDecision d = evaluate_promotion(e);
    EXPECT_TRUE(d.promoted);
    EXPECT_FALSE(d.default_enabled);
    ASSERT_FALSE(d.reasons.empty());
    EXPECT_NE(d.reasons.back().find("NOT RUN"), std::string::npos);
    e.upstream_status = "measured";
    e.upstream_ratio = 1.3;
    d = evaluate_promotion(e);
    EXPECT_TRUE(d.default_enabled);
    EXPECT_TRUE(d.reasons.empty());
    // "measured" without a ratio is not evidence
    e.upstream_ratio.reset();
    EXPECT_FALSE(evaluate_promotion(e).default_enabled);
}

TEST(Planner, ShapeBucketsArePowersOfTwo) {
    EXPECT_EQ(shape_bucket({256, 1024, 256}), "m256n1024k256");
    EXPECT_EQ(shape_bucket({67, 131, 95}), "m128n256k128");
    EXPECT_EQ(shape_bucket({1, 1, 1}), "m1n1k1");
    EXPECT_EQ(shape_bucket({32, 4096, 0}), "m32n4096k0");
}

TEST(Planner, CatalogIsConsistent) {
    const auto& cat = kernel_catalog();
    EXPECT_EQ(cat.size(), 8u);
    for (const auto& k : cat) {
        EXPECT_NE(find_kernel(k.id), nullptr);
        EXPECT_FALSE(path_key(k).empty());
    }
    EXPECT_EQ(path_key(*find_kernel("gemm_f32_tiled")), "vulkan_tiled:gemm:f32");
    EXPECT_EQ(path_key(*find_kernel("gemm_i8dot_tiled")), "vulkan_intdot:gemm:i8");
    EXPECT_EQ(path_key(*find_kernel("gemm_coopmat_f16")), "vulkan_coopmat:gemm:f16");
    EXPECT_EQ(path_key(*find_kernel("rope_rows")), "vulkan_scalar:rope:f32");
    EXPECT_EQ(find_kernel("nope"), nullptr);
}

TEST(PlannerReport, DoctorSectionShowsUnknownWhenKernelsAreNotBuiltIn) {
    const std::vector<DeviceCaps> devs = {caps_680m()};
    const json r = planner_report(devs, nullptr, {});
    EXPECT_NE(r["kernel_hashes"].get<std::string>().find("unknown"), std::string::npos);
    ASSERT_EQ(r["devices"].size(), 1u);
    for (const auto& p : r["devices"][0]["plans"]) EXPECT_EQ(p["path"], "reference");
    EXPECT_EQ(r["profile"]["path"], "unknown");
    EXPECT_FALSE(r["policy"]["allow_unbaselined"].get<bool>());
    // every catalog kernel is listed with a gate verdict
    EXPECT_EQ(r["devices"][0]["kernels"].size(), kernel_catalog().size());
}
