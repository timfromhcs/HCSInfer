#pragma once
// Small value types shared across the control plane and engine boundary.
#include <compare>
#include <cstdint>
#include <string>

namespace hcs {

struct JobId {
    std::uint64_t value = 0;  // 0 == invalid
    bool valid() const noexcept { return value != 0; }
    auto operator<=>(const JobId&) const = default;
};

inline std::string to_string(JobId id) { return "job-" + std::to_string(id.value); }

}  // namespace hcs
