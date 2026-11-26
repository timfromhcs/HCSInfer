#pragma once
// Internal to hcs_cli: `HCSInfer models load|unload|status` - an HTTP client of a running `HCSInfer serve`
// (a model lives in the serving process, not in a one-shot CLI invocation).
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::cli {

const char* models_remote_help();
// `sub` is "load", "unload" or "status"; `args` are the remaining arguments after it.
int run_models_remote(const std::string& sub, std::vector<std::string> args, std::ostream& out,
                      std::ostream& err);

}  // namespace hcs::cli
