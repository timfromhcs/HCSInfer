#include <gtest/gtest.h>

#include <limits>

#include "hcs/memory_budget.hpp"

using namespace hcs;
constexpr std::uint64_t MiB = 1ull << 20;
constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();

TEST(MemoryBudgetMath, PhysicalOnly) {
    MemoryMeasurements m{10000 * MiB, 2000 * MiB, 500 * MiB, std::nullopt};
    EXPECT_EQ(compute_application_budget(m), 7500 * MiB);
}

TEST(MemoryBudgetMath, VulkanHeadroomCapsBudget) {
    MemoryMeasurements m{10000 * MiB, 2000 * MiB, 500 * MiB, 4000 * MiB};
    EXPECT_EQ(compute_application_budget(m), 4000 * MiB);
    m.vulkan_headroom_bytes = 9000 * MiB;  // larger than host limit => host limit wins
    EXPECT_EQ(compute_application_budget(m), 7500 * MiB);
}

TEST(MemoryBudgetMath, SaturatesInsteadOfUnderflowing) {
    MemoryMeasurements m{1000, 5000, 5000, std::nullopt};
    EXPECT_EQ(compute_application_budget(m), 0u);
    m = {kMax, kMax, kMax, std::nullopt};
    EXPECT_EQ(compute_application_budget(m), 0u);
    m = {kMax, 0, 0, std::nullopt};
    EXPECT_EQ(compute_application_budget(m), kMax);
    m = {1000, 0, 0, 0};
    EXPECT_EQ(compute_application_budget(m), 0u);
}

TEST(MemoryBudget, ReserveAndRelease) {
    MemoryBudget b(1000);
    ASSERT_TRUE(b.reserve("w", MemoryCategory::ModelWeights, 600));
    ASSERT_TRUE(b.reserve("kv", MemoryCategory::KvCache, 300));
    EXPECT_EQ(b.reserved(), 900u);
    EXPECT_EQ(b.remaining(), 100u);
    EXPECT_EQ(b.reserved_for(MemoryCategory::KvCache), 300u);
    ASSERT_TRUE(b.release("kv"));
    EXPECT_EQ(b.remaining(), 400u);
    EXPECT_EQ(b.release("kv").error().code, ErrorCode::NotFound);
}

TEST(MemoryBudget, ExceedingBudgetFailsAndLeavesStateUntouched) {
    MemoryBudget b(1000);
    ASSERT_TRUE(b.reserve("a", MemoryCategory::ModelWeights, 900));
    auto r = b.reserve("b", MemoryCategory::Workspace, 101);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(b.reserved(), 900u);
    EXPECT_EQ(b.entries().size(), 1u);
    EXPECT_TRUE(b.reserve("b", MemoryCategory::Workspace, 100));  // exact fit is allowed
}

TEST(MemoryBudget, OverflowIsOutOfMemoryNotWraparound) {
    MemoryBudget b(kMax);
    ASSERT_TRUE(b.reserve("a", MemoryCategory::ModelWeights, kMax - 10));
    auto r = b.reserve("b", MemoryCategory::Workspace, 11);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(b.reserved(), kMax - 10);
    EXPECT_FALSE(b.can_fit(kMax));
    EXPECT_TRUE(b.can_fit(10));
    EXPECT_EQ(b.reserve("c", MemoryCategory::Workspace, kMax).error().code, ErrorCode::OutOfMemory);
}

TEST(MemoryBudget, NoDoubleCountingOfSharedRegion) {
    MemoryBudget b(1000);
    ASSERT_TRUE(b.reserve("weights#0", MemoryCategory::ModelWeights, 400, MemoryPlacement::SharedUma));
    // The same UMA region viewed again as a Vulkan buffer must NOT be reserved a second time.
    auto dup = b.reserve("weights#0", MemoryCategory::VulkanBuffers, 400, MemoryPlacement::SharedUma);
    ASSERT_FALSE(dup);
    EXPECT_EQ(dup.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(b.reserved(), 400u);
    EXPECT_EQ(b.reserved_for(MemoryCategory::VulkanBuffers), 0u);
}

TEST(MemoryBudget, InvalidArguments) {
    MemoryBudget b(100);
    EXPECT_EQ(b.reserve("", MemoryCategory::Runtime, 1).error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(b.reserve("z", MemoryCategory::Runtime, 0).error().code, ErrorCode::InvalidArgument);
}

TEST(MemoryBudget, Resize) {
    MemoryBudget b(1000);
    ASSERT_TRUE(b.reserve("kv", MemoryCategory::KvCache, 400));
    ASSERT_TRUE(b.reserve("w", MemoryCategory::ModelWeights, 500));
    EXPECT_EQ(b.resize("kv", 501).error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(b.reserved(), 900u);
    ASSERT_TRUE(b.resize("kv", 500));
    EXPECT_EQ(b.reserved(), 1000u);
    ASSERT_TRUE(b.resize("kv", 100));
    EXPECT_EQ(b.reserved(), 600u);
    EXPECT_EQ(b.resize("nope", 1).error().code, ErrorCode::NotFound);
    EXPECT_EQ(b.resize("kv", 0).error().code, ErrorCode::InvalidArgument);
}

TEST(MemoryBudget, RemeasureCanOvercommit) {
    MemoryBudget b(1000);
    ASSERT_TRUE(b.reserve("w", MemoryCategory::ModelWeights, 800));
    b.update_budget(MemoryMeasurements{500, 0, 0, std::nullopt});
    EXPECT_TRUE(b.over_committed());
    EXPECT_EQ(b.remaining(), 0u);
    EXPECT_FALSE(b.can_fit(1));
    EXPECT_EQ(b.reserve("x", MemoryCategory::Workspace, 1).error().code, ErrorCode::OutOfMemory);
    ASSERT_TRUE(b.release("w"));
    EXPECT_FALSE(b.over_committed());
    EXPECT_EQ(b.remaining(), 500u);
}

TEST(MemoryBudget, ConstructFromMeasurements) {
    MemoryBudget b(MemoryMeasurements{2000, 500, 500, std::nullopt});
    EXPECT_EQ(b.total(), 1000u);
}
