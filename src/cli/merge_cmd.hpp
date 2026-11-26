#pragma once
// Internal to hcs_cli: `HCSInfer merge ...` (streaming model merge, see hcs/merge.hpp).
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::cli {

const char* merge_help();
int run_merge_cmd(std::vector<std::string> args, std::ostream& out, std::ostream& err);

}  // namespace hcs::cli
