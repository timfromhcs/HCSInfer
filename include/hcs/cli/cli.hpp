#pragma once
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::cli {

// Runs the CLI. `args` excludes argv[0]. Returns the process exit code
// (0 = ok; otherwise hcs::exit_code(ErrorCode), see include/hcs/result.hpp).
int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

// Text of the root help (exposed for tests/docs).
std::string root_help();

}  // namespace hcs::cli
