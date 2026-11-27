#pragma once
// `HCSInfer image generate|edit` - stable-diffusion.cpp adapter only (no sd_* calls here).
#include <ostream>
#include <string>
#include <vector>

namespace hcs::cli {

const char* image_help();
// `image upscale` (upscale_cmd.cpp): works without a diffusion engine.
const char* image_upscale_help();
int run_image_upscale(std::vector<std::string> args, std::ostream& out, std::ostream& err);
int run_image(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
              std::ostream& err);

}  // namespace hcs::cli
