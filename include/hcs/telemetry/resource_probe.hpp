#pragma once
// Live resource measurements for the overload controller (AGENTS.md 10.1, plan F1). Read-only OS queries
// routed through the hardware module. A figure that cannot be measured is nullopt and rendered as the string
// "unknown" - it is never guessed or defaulted.
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

namespace hcs::telemetry {

struct ResourceSample {
    std::optional<std::uint64_t> total_physical_bytes;
    std::optional<std::uint64_t> free_physical_bytes;  // what the OS can hand out now (free + reclaimable)
    std::optional<std::uint64_t> commit_used_bytes;
    std::optional<std::uint64_t> commit_limit_bytes;
    std::optional<std::uint64_t> process_working_set_bytes;
    // OS GPU memory headroom (DXGI budget - usage); a proxy for the Vulkan heap budget, see
    // hcs::hardware::sample_gpu_heap_headroom().
    std::optional<std::uint64_t> gpu_heap_headroom_bytes;

    // used / limit; nullopt when either is unknown or the limit is zero.
    std::optional<double> commit_fraction() const noexcept;
};

using ResourceProbe = std::function<ResourceSample()>;

// {"free_physical_bytes": N | "unknown", ...}
nlohmann::json to_json(const ResourceSample& s);

// The live probe. Without the hardware module in this build every figure is unknown. The GPU figure is cached
// for ~1 s (a DXGI factory per sample would be wasteful at the watchdog rate).
ResourceProbe make_system_probe();

}  // namespace hcs::telemetry
