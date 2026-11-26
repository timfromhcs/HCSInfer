#pragma once
// `HCSInfer tune`: benchmark every HCS Vulkan kernel on this machine against the CPU oracle (correctness)
// and a multi-threaded CPU baseline (speed), and persist the keyed profile cache the planner reads
// (hcs/hardware/kernel_profile.hpp). Nothing here changes any default; see docs/vulkan-kernels.md.
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::kernels::vulkan {

// Exit codes (hcs/result.hpp): 0 ok, 2 invalid argument, 4 a kernel produced wrong results or the device
// failed, 10 no usable Vulkan device. `args` are the arguments after `tune`.
int run_tune(std::vector<std::string> args, const std::filesystem::path& state_dir, std::ostream& out,
             std::ostream& err);

const char* tune_help_text();

}  // namespace hcs::kernels::vulkan
