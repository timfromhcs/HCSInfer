#pragma once
// Internal to hcs_cli: `HCSInfer train ...` (HCS Train pipeline, eval gate and adapter bookkeeping, ADR-012).
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::cli {

const char* train_help();
int run_train(std::vector<std::string> args, const std::filesystem::path& state_dir, std::ostream& out,
              std::ostream& err);

}  // namespace hcs::cli
