// Memory measurement feeding the budget (M10 audit D4). The live test reports GTEST_SKIP when the OS query is
// unavailable; it never fakes a pass.
#include <gtest/gtest.h>

#include "hcs/hardware/memory_probe.hpp"

using namespace hcs;
using namespace hcs::hardware;

namespace {
constexpr std::uint64_t kGiB = 1ull << 30;
}

TEST(MemoryProbe, PolicyReservesTenPercentWithAOneGiBFloorAndHalfGiBMargin) {
    auto big = make_measurements(16 * kGiB, 10 * kGiB);
    EXPECT_EQ(big.available_physical_bytes, 10 * kGiB);
    EXPECT_EQ(big.os_reserve_bytes, (16 * kGiB) / 10);
    EXPECT_EQ(big.safety_margin_bytes, kGiB / 2);
    EXPECT_FALSE(big.vulkan_headroom_bytes.has_value());
    auto small = make_measurements(4 * kGiB, 3 * kGiB);
    EXPECT_EQ(small.os_reserve_bytes, kGiB);  // floor
    // available can never exceed installed RAM even if the OS reports garbage
    EXPECT_EQ(make_measurements(8 * kGiB, 99 * kGiB).available_physical_bytes, 8 * kGiB);
}

TEST(MemoryProbe, BudgetNeverUnderflowsWhenTheMachineIsAlmostFull) {
    auto m = make_measurements(16 * kGiB, kGiB / 4);  // 256 MiB free: below reserve + margin
    EXPECT_EQ(compute_application_budget(m), 0u);
    auto ok = make_measurements(16 * kGiB, 10 * kGiB);
    EXPECT_EQ(compute_application_budget(ok), 10 * kGiB - (16 * kGiB) / 10 - kGiB / 2);
}

TEST(MemoryProbe, LiveMeasurementIsPlausible) {
    auto m = measure_memory();
    if (!m) GTEST_SKIP() << "NOT RUN: OS memory query unavailable here";
    EXPECT_GT(m->available_physical_bytes, 0u);
    EXPECT_GE(m->os_reserve_bytes, kGiB);
    EXPECT_GT(compute_application_budget(*m), 0u) << "this machine has no usable headroom at all";
}
