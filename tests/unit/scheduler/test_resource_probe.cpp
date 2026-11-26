// Resource sample JSON + the live system probe. The live test reports GTEST_SKIP when the OS gives no figure.
#include <gtest/gtest.h>

#include "hcs/telemetry/resource_probe.hpp"

using namespace hcs::telemetry;

TEST(ResourceSample, UnknownFiguresRenderAsTheStringUnknown) {
    ResourceSample s;
    s.free_physical_bytes = 123;
    const auto j = to_json(s);
    EXPECT_EQ(j["free_physical_bytes"], 123);
    EXPECT_EQ(j["commit_used_bytes"], "unknown");
    EXPECT_EQ(j["gpu_heap_headroom_bytes"], "unknown");
    EXPECT_EQ(j["commit_fraction"], "unknown");
}

TEST(ResourceSample, CommitFractionNeedsBothFiguresAndANonZeroLimit) {
    ResourceSample s;
    EXPECT_FALSE(s.commit_fraction().has_value());
    s.commit_used_bytes = 50;
    EXPECT_FALSE(s.commit_fraction().has_value());
    s.commit_limit_bytes = 0;
    EXPECT_FALSE(s.commit_fraction().has_value());
    s.commit_limit_bytes = 200;
    ASSERT_TRUE(s.commit_fraction().has_value());
    EXPECT_DOUBLE_EQ(*s.commit_fraction(), 0.25);
}

TEST(SystemProbe, LiveSampleIsPlausible) {
    const auto s = make_system_probe()();
    if (!s.free_physical_bytes)
        GTEST_SKIP() << "no OS memory figure available in this build/environment (reported as unknown)";
    ASSERT_TRUE(s.total_physical_bytes.has_value());
    EXPECT_GT(*s.free_physical_bytes, 0u);
    EXPECT_LE(*s.free_physical_bytes, *s.total_physical_bytes);
    if (s.commit_used_bytes && s.commit_limit_bytes) {
        EXPECT_GT(*s.commit_limit_bytes, 0u);
        EXPECT_GT(*s.commit_used_bytes, 0u);
    }
    if (s.process_working_set_bytes)
        EXPECT_GT(*s.process_working_set_bytes, 0u);
}
