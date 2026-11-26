#pragma once
// Internal: decode one device record of the doctor's Vulkan probe (vulkan_info.cpp) into a DeviceCaps, and
// export a DeviceCaps back to JSON for `doctor --json`.
#include <filesystem>
#include <vector>

#include "hcs/hardware/caps.hpp"
#include "hcs/hardware/planner.hpp"
#include "hw.hpp"

namespace hcs::hardware {

// `device` is one element of vulkan_report()["devices"]. Missing or "unknown" fields stay Unknown/nullopt.
DeviceCaps caps_from_probe(const json& device);
std::vector<DeviceCaps> caps_from_report(const json& vulkan_report);

// Typed view for the report: tri-state strings ("yes"/"no"/"unknown"), numbers or "unknown".
json caps_to_json(const DeviceCaps& caps);

// Planner section of `doctor --json` (planner_json.cpp). `hashes` == nullptr means the kernel binaries are
// not part of this build (every kernel hash is then unknown and every plan is the CPU reference).
json planner_report(const std::vector<DeviceCaps>& devices, const KernelHashes* hashes,
                    const std::filesystem::path& state_dir);

}  // namespace hcs::hardware
