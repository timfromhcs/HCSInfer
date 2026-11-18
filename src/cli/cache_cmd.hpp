#pragma once
// Internal to hcs_cli: `HCSInfer cache stats|clear` (HCS-owned state-directory caches only).
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::cli {

const char* cache_help();
int run_cache(std::vector<std::string> args, const std::filesystem::path& state_dir, std::ostream& out,
              std::ostream& err);

}  // namespace hcs::cli
