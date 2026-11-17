#include <gtest/gtest.h>

#include <limits>

#include "hcs/checked_math.hpp"

using namespace hcs;
constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();

TEST(CheckedMath, Add) {
    EXPECT_EQ(checked_add(1, 2), 3u);
    EXPECT_EQ(checked_add(kMax, 0), kMax);
    EXPECT_FALSE(checked_add(kMax, 1).has_value());
    EXPECT_FALSE(checked_add(kMax, kMax).has_value());
}

TEST(CheckedMath, Mul) {
    EXPECT_EQ(checked_mul(0, kMax), 0u);
    EXPECT_EQ(checked_mul(1ull << 31, 1ull << 31), 1ull << 62);
    EXPECT_FALSE(checked_mul(1ull << 32, 1ull << 32).has_value());
    EXPECT_FALSE(checked_mul(kMax, 2).has_value());
}

TEST(CheckedMath, SaturatingSub) {
    EXPECT_EQ(saturating_sub(5, 3), 2u);
    EXPECT_EQ(saturating_sub(3, 5), 0u);
    EXPECT_EQ(saturating_sub(0, kMax), 0u);
}

TEST(CheckedMath, RangeWithin) {
    EXPECT_TRUE(range_within(0, 10, 10));
    EXPECT_FALSE(range_within(1, 10, 10));
    EXPECT_FALSE(range_within(kMax, 2, kMax));  // would wrap
    EXPECT_FALSE(range_within(2, kMax, 100));
    EXPECT_TRUE(range_within(10, 0, 10));
}
