#pragma once
// Overflow-safe unsigned 64-bit arithmetic used by budgets, parsers and offset math.
#include <cstdint>
#include <limits>
#include <optional>

namespace hcs {

constexpr std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
    if (a > std::numeric_limits<std::uint64_t>::max() - b)
        return std::nullopt;
    return a + b;
}

constexpr std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) noexcept {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
        return std::nullopt;
    return a * b;
}

constexpr std::uint64_t saturating_sub(std::uint64_t a, std::uint64_t b) noexcept {
    return a > b ? a - b : 0;
}

// True iff [offset, offset+size) lies inside [0, limit) with no overflow.
constexpr bool range_within(std::uint64_t offset, std::uint64_t size, std::uint64_t limit) noexcept {
    auto end = checked_add(offset, size);
    return end.has_value() && *end <= limit;
}

}  // namespace hcs
