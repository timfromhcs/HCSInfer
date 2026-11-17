#pragma once
// Internal to hcs_cli: `HCSInfer benchmark --suite quick` (real text-model benchmark, AGENTS.md 21.1 JSON).
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::cli {

const char* benchmark_help();
int run_benchmark(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
                  std::ostream& err);

}  // namespace hcs::cli
