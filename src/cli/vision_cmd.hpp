#pragma once
// Internal to hcs_cli: the `vision` command (llama.cpp + libmtmd through the engine adapter).
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::cli {

const char* vision_help();
int run_vision(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
               std::ostream& err);

}  // namespace hcs::cli
