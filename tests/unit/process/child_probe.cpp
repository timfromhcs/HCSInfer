// Test helper spawned by the hcs::process unit tests. It is NOT a test itself.
//
//   child_probe echo <words...>            print the words to stdout and a line to stderr, exit 0
//   child_probe exit <n>                   exit with code n
//   child_probe sleep <ms>                 sleep, exit 0
//   child_probe spam <kb>                  write <kb> KiB of 'x' to stdout, exit 0
//   child_probe sleepwrite <ms> <file>     sleep, then create <file>, exit 0
//   child_probe tree <ms> <file>           spawn a detached `sleepwrite <ms> <file>` grandchild, then sleep
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int arg_int(const std::vector<std::string>& a, std::size_t i, int fallback) {
    return i < a.size() ? std::atoi(a[i].c_str()) : fallback;
}

std::string arg_str(const std::vector<std::string>& a, std::size_t i) {
    return i < a.size() ? a[i] : std::string();
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> a(argv, argv + argc);
    if (a.size() < 2) {
        std::fputs("usage: child_probe <cmd> ...\n", stderr);
        return 2;
    }
    const std::string cmd = a[1];

    if (cmd == "echo") {
        for (std::size_t i = 2; i < a.size(); ++i) {
            std::fputs(a[i].c_str(), stdout);
            std::fputc(' ', stdout);
        }
        std::fputs("\nto-stderr\n", stdout);
        std::fflush(nullptr);
        return 0;
    }
    if (cmd == "exit") {
        return arg_int(a, 2, 0);
    }
    if (cmd == "sleep") {
        sleep_ms(arg_int(a, 2, 1000));
        return 0;
    }
    if (cmd == "env") {
        const std::string name = arg_str(a, 2);
        const char* value = name.empty() ? nullptr : std::getenv(name.c_str());
        std::fputs(value != nullptr ? value : "(unset)", stdout);
        std::fputc('\n', stdout);
        std::fflush(nullptr);
        return 0;
    }
    if (cmd == "spam") {
        const int kb = arg_int(a, 2, 8);
        const std::string block(1024, 'x');
        for (int i = 0; i < kb; ++i) {
            std::fputs(block.c_str(), stdout);
        }
        std::fflush(nullptr);
        return 0;
    }
    if (cmd == "sleepwrite") {
        sleep_ms(arg_int(a, 2, 1000));
        const std::string path = arg_str(a, 3);
        if (!path.empty()) {
            std::ofstream out(path, std::ios::binary);
            out << "alive";
        }
        return 0;
    }
    if (cmd == "tree") {
        const int ms = arg_int(a, 2, 1000);
        const std::string marker = arg_str(a, 3);
#ifdef _WIN32
        std::string command_line =
            "\"" + a[0] + "\" sleepwrite " + std::to_string(ms) + " \"" + marker + "\"";
        std::vector<char> mutable_command(command_line.begin(), command_line.end());
        mutable_command.push_back('\0');
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
#endif
        std::fputs("spawned\n", stdout);
        std::fflush(nullptr);
        sleep_ms(ms + 2000);
        return 0;
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 3;
}
