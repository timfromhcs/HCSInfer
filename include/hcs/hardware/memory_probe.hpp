#pragma once
// Live physical-memory measurement feeding the global memory budget (AGENTS.md 10.1, M10 audit D4).
// Read-only OS query: Windows GlobalMemoryStatusEx, Linux /proc/meminfo MemAvailable.
//
// The returned MemoryMeasurements are inputs to hcs::compute_application_budget():
//   available_physical_bytes : memory the OS can hand out right now (free + reclaimable cache)
//   os_reserve_bytes         : kept for the OS and other programs: max(1 GiB, 10 % of installed RAM)
//   safety_margin_bytes      : 512 MiB slack for allocator overhead, graph workspace and GUI/API
//   vulkan_headroom_bytes    : NOT filled in yet (needs a Vulkan device query); on a UMA iGPU the heap is carved
//                              out of the same RAM, so the physical figure is the binding one.
#include <optional>

#include "hcs/memory_budget.hpp"

namespace hcs::hardware {

// nullopt when the OS query fails (the budget then stays as it was; nothing is guessed).
std::optional<MemoryMeasurements> measure_memory();

// Pure policy so it can be unit-tested: derives reserve/margin from the installed RAM size.
MemoryMeasurements make_measurements(std::uint64_t total_physical_bytes, std::uint64_t available_physical_bytes);

}  // namespace hcs::hardware
