#include <iostream>
#include <string>
#include <vector>

#include "hcs/cli/cli.hpp"

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    // An escaping exception would end in std::terminate with no message at all; report it instead. Arguments
    // are never echoed, so nothing sensitive is printed.
    try {
        return hcs::cli::run(args, std::cout, std::cerr);
    } catch (const std::exception& e) {
        std::cerr << "error: internal error: " << e.what() << "\n";
        return 70;  // EX_SOFTWARE
    } catch (...) {
        std::cerr << "error: internal error (non-standard exception)\n";
        return 70;
    }
}
