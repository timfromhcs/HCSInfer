#pragma once
// Internal to hcs_cli: the `chat` command (llama text engine adapter).
#include <iosfwd>
#include <string>
#include <vector>

namespace hcs::cli {

const char* chat_help();
int run_chat(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
             std::ostream& err);

}  // namespace hcs::cli
