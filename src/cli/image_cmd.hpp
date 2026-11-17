#pragma once
// `HCSInfer image generate|edit` - stable-diffusion.cpp adapter only (no sd_* calls here).
#include <ostream>
#include <string>
#include <vector>

namespace hcs::cli {

const char* image_help();
int run_image(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
              std::ostream& err);

}  // namespace hcs::cli
