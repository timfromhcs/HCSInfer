// Kernel profile cache: key matching, persistence, invalidation, "never persist a failing kernel".
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "planner_fixtures.hpp"
#include "temp_dir.hpp"

using namespace hcs::test;

TEST(KernelProfile, UpsertRejectsEntriesThatFailedCorrectness) {
    const DeviceCaps c = caps_680m();
    KernelProfile p;
    ProfileEntry bad = good_entry(c, "gemm_f32_tiled", "h", {64, 64, 64});
    bad.correct = false;
    const auto r = p.upsert(bad);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, hcs::ErrorCode::InvalidArgument);
    EXPECT_TRUE(p.entries().empty());
    ProfileEntry incomplete = good_entry(c, "gemm_f32_tiled", "", {64, 64, 64});
    EXPECT_FALSE(p.upsert(incomplete).ok());
}

TEST(KernelProfile, UpsertReplacesTheSameKeyAndFindIsExact) {
    const DeviceCaps c = caps_680m();
    KernelProfile p;
    const Shape s{256, 1024, 256};
    ProfileEntry a = good_entry(c, "gemm_f32_tiled", "h1", s);
    ASSERT_TRUE(p.upsert(a).ok());
    a.wall_ms_median = 0.5;
    ASSERT_TRUE(p.upsert(a).ok());
    ASSERT_EQ(p.entries().size(), 1u);
    EXPECT_DOUBLE_EQ(p.find(c.device_key(), "gemm_f32_tiled", "h1", shape_bucket(s))->wall_ms_median, 0.5);
    EXPECT_EQ(p.find(c.device_key(), "gemm_f32_tiled", "h2", shape_bucket(s)), nullptr);
    EXPECT_EQ(p.find("vk:1002:1681:drv=other:api=1.4.315", "gemm_f32_tiled", "h1", shape_bucket(s)), nullptr);
    EXPECT_EQ(p.find(c.device_key(), "gemm_f32_tiled", "h1", "m1n1k1"), nullptr);
    EXPECT_TRUE(p.has_stale(c.device_key(), "gemm_f32_tiled", "h2", shape_bucket(s)));
    EXPECT_FALSE(p.has_stale(c.device_key(), "gemm_f32_tiled", "h1", shape_bucket(s)));
    // a different hash is a separate entry, not an overwrite
    ASSERT_TRUE(p.upsert(good_entry(c, "gemm_f32_tiled", "h2", s)).ok());
    EXPECT_EQ(p.entries().size(), 2u);
}

TEST(KernelProfile, JsonRoundTripPreservesEveryField) {
    const DeviceCaps c = caps_680m();
    KernelProfile p;
    ProfileEntry e = good_entry(c, "gemm_f32_tiled", "h1", {256, 1024, 256});
    e.upstream_status = "measured";
    e.upstream_ratio = 1.4;
    e.gpu_ms_median.reset();
    e.gpu_spread.reset();
    ASSERT_TRUE(p.upsert(e).ok());
    ASSERT_TRUE(p.upsert(good_entry(c, "softmax_rows", "h3", {32, 4096, 0})).ok());
    auto back = KernelProfile::from_json_text(p.to_json_text());
    ASSERT_TRUE(back.ok()) << back.error().message;
    ASSERT_EQ(back.value().entries().size(), 2u);
    const ProfileEntry* r = back.value().find(c.device_key(), "gemm_f32_tiled", "h1", e.shape_bucket);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->m, 256u);
    EXPECT_EQ(r->identical_runs, 3u);
    EXPECT_EQ(r->upstream_status, "measured");
    EXPECT_DOUBLE_EQ(*r->upstream_ratio, 1.4);
    EXPECT_FALSE(r->gpu_ms_median.has_value());
    EXPECT_DOUBLE_EQ(r->baseline_ms, 2.0);
    EXPECT_EQ(r->baseline_name, "cpu_mt_autovec");
    EXPECT_EQ(back.value().to_json_text(), p.to_json_text());
}

TEST(KernelProfile, CorruptWrongSchemaAndFailingEntriesAreErrors) {
    EXPECT_FALSE(KernelProfile::from_json_text("not json").ok());
    EXPECT_FALSE(KernelProfile::from_json_text("[]").ok());
    EXPECT_FALSE(KernelProfile::from_json_text(R"({"schema_version":99,"entries":[]})").ok());
    EXPECT_FALSE(KernelProfile::from_json_text(R"({"schema_version":1})").ok());
    EXPECT_FALSE(
        KernelProfile::from_json_text(R"({"schema_version":1,"entries":[{"device_key":"x"}]})").ok());
    EXPECT_TRUE(KernelProfile::from_json_text(R"({"schema_version":1,"entries":[]})").ok());
}

TEST(KernelProfile, FileRoundTripMissingFileIsEmptyAndCorruptFileIsNotReplaced) {
    hcs::test::TempDir dir;
    const auto path = profile_path(dir.path());
    auto missing = load_profile(path);
    ASSERT_TRUE(missing.ok());
    EXPECT_TRUE(missing.value().entries().empty());

    const DeviceCaps c = caps_680m();
    KernelProfile p;
    ASSERT_TRUE(p.upsert(good_entry(c, "rope_rows", "h", {512, 128, 0})).ok());
    ASSERT_TRUE(save_profile(path, p).ok());
    auto back = load_profile(path);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value().entries().size(), 1u);

    {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << "{ corrupt";
    }
    auto bad = load_profile(path);
    ASSERT_FALSE(bad.ok());  // the caller decides; load never silently discards a corrupt file
    EXPECT_EQ(bad.error().code, hcs::ErrorCode::InvalidArgument);
}

TEST(KernelProfile, ResetAndPruneStale) {
    const DeviceCaps c = caps_680m();
    KernelProfile p;
    ASSERT_TRUE(p.upsert(good_entry(c, "gemm_f32_tiled", "old", {64, 64, 64})).ok());
    ASSERT_TRUE(p.upsert(good_entry(c, "gemm_f32_tiled", "new", {64, 64, 64})).ok());
    ASSERT_TRUE(p.upsert(good_entry(c, "softmax_rows", "s1", {8, 8, 0})).ok());
    DeviceCaps other = c;
    other.driver_version_raw = "0x1";
    ASSERT_TRUE(p.upsert(good_entry(other, "gemm_f32_tiled", "old", {64, 64, 64})).ok());
    ASSERT_EQ(p.entries().size(), 4u);

    // Only the live device's stale-hash entry (and kernels that no longer exist) go; other devices stay.
    const std::size_t pruned = p.prune_stale(c.device_key(), {{"gemm_f32_tiled", "new"}});
    EXPECT_EQ(pruned, 2u);  // gemm "old" and softmax (kernel no longer exists in the live list)
    EXPECT_EQ(p.entries().size(), 2u);

    EXPECT_EQ(p.reset(c.device_key(), "gemm_f32_tiled"), 1u);
    EXPECT_EQ(p.entries().size(), 1u);
    EXPECT_EQ(p.reset("vk:1002:1681", ""), 1u);  // prefix: all drivers of this device
    EXPECT_TRUE(p.entries().empty());
}

TEST(KernelProfile, Fnv1aIsStableAndSensitive) {
    EXPECT_EQ(fnv1a64_hex("", 0), "cbf29ce484222325");   // FNV-1a/64 of the empty input (offset basis)
    EXPECT_EQ(fnv1a64_hex("a", 1), "af63dc4c8601ec8c");  // published FNV-1a/64 test vector
    EXPECT_NE(fnv1a64_hex("ab", 2), fnv1a64_hex("ba", 2));
}
