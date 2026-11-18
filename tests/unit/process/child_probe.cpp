// Test helper spawned by the hcs::process unit tests. It is NOT a test itself.
//
//   child_probe echo <words...>            print the words to stdout and a line to stderr, exit 0
//   child_probe exit <n>                   exit with code n
//   child_probe sleep <ms>                 sleep, exit 0
//   child_probe spam <kb>                  write <kb> KiB of 'x' to stdout, exit 0
//   child_probe sleepwrite <ms> <file>     sleep, then create <file>, exit 0
//   child_probe tree <ms> <file>           spawn a detached `sleepwrite <ms> <file>` grandchild, then sleep
//   child_probe streams                    out1 -> stdout, err1 -> stderr, out2 -> stdout, exit 0
//   child_probe envnames                   print the NAMES (never values) of the environment variables, one
//   per line child_probe alloc <mb>                 commit <mb> MiB, print alloc-ok (or alloc-failed), then
//   sleep 5 s child_probe fork <n> <ms> <prefix>     spawn <n> `sleepwrite <ms> <prefix><i>` children, then
//   sleep ms+1.5 s child_probe --offline ... -- <script>  agent simulator for the session-manager tests (see
//   agent_sim)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
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

std::string json_escape(const std::string& in) {
    std::string out;
    for (const char c : in) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void emit(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void emit_text_turn(const std::string& text) {
    emit("{\"type\":\"turn_end\",\"message\":{\"role\":\"assistant\",\"content\":\"" + json_escape(text) +
         "\",\"stopReason\":\"stop\",\"usage\":{\"input\":10,\"output\":5}}}");
}

void emit_tool_call(const std::string& code) {
    emit(
        "{\"type\":\"tool_execution_start\",\"toolName\":\"python_repl\",\"toolCallId\":\"c1\",\"args\":{"
        "\"code\":\"" +
        json_escape(code) + "\"}}");
}

void spawn_sleeper(const std::string& self, const std::string& marker, int ms) {
#ifdef _WIN32
    std::string command_line = "\"" + self + "\" sleepwrite " + std::to_string(ms) + " \"" + marker + "\"";
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    (void)self;
    (void)marker;
    (void)ms;
#endif
}

// Stands in for prime-agent: `child_probe --offline --provider hcs ... -- <script>`. The last argument is the
// task, which the tests use as the script name. Prints the same JSON-lines events prime-agent's `--mode json`
// does.
int agent_sim(const std::vector<std::string>& a) {
    const std::string script = a.back();
    const char* token_env = std::getenv("HCS_AGENT_TOKEN");
    const std::string token = token_env != nullptr ? token_env : "";
    if (script == "ok") {
        emit("{\"type\":\"agent_start\"}");
        std::fputs("warming up\n", stderr);
        std::fflush(stderr);
        emit("this line is not json and must be ignored");
        emit_tool_call("print(1)");
        emit(
            "{\"type\":\"tool_execution_end\",\"toolName\":\"python_repl\",\"toolCallId\":\"c1\",\"isError\":"
            "false,"
            "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"1\"}],\"details\":{\"durationMs\":3}}}");
        emit_text_turn("all done");
        emit("{\"type\":\"agent_end\"}");
        return 0;
    }
    if (script == "violation") {
        emit("{\"type\":\"agent_start\"}");
        emit_tool_call("import socket");
        sleep_ms(30000);
        return 0;
    }
    if (script == "hang") {
        emit("{\"type\":\"agent_start\"}");
        sleep_ms(60000);
        return 0;
    }
    if (script == "flood") {
        emit("{\"type\":\"agent_start\"}");
        for (int i = 0; i < 3000; ++i) {
            emit_text_turn("event number " + std::to_string(i) + std::string(200, 'x'));
        }
        emit("{\"type\":\"agent_end\"}");
        return 0;
    }
    if (script == "bigline") {
        emit("{\"type\":\"agent_start\"}");
        const std::string junk(2 * 1024 * 1024, 'j');
        emit(junk);
        emit_text_turn("after the big line");
        emit("{\"type\":\"agent_end\"}");
        return 0;
    }
    if (script == "envreport") {
        std::string names;
        bool leaked_in_other_var = false;
        bool token_in_argv = false;
        for (const auto& arg : a) {
            if (!token.empty() && arg.find(token) != std::string::npos) {
                token_in_argv = true;
            }
        }
#ifdef _WIN32
        if (LPCH block = GetEnvironmentStringsA()) {
            for (const char* cursor = block; *cursor != '\0';) {
                const std::string entry(cursor);
                cursor += entry.size() + 1;
                const std::size_t eq = entry.find('=');
                if (eq == std::string::npos || eq == 0) {
                    continue;
                }
                const std::string name = entry.substr(0, eq);
                names += name + ",";
                if (name != "HCS_AGENT_TOKEN" && !token.empty() && entry.find(token) != std::string::npos) {
                    leaked_in_other_var = true;
                }
            }
            FreeEnvironmentStringsA(block);
        }
#endif
        emit("{\"type\":\"agent_start\"}");
        emit_text_turn("env=" + names + " token_in_env=" + (token.empty() ? "0" : "1") +
                       " token_in_argv=" + (token_in_argv ? "1" : "0") + " token_in_other_var=" +
                       (leaked_in_other_var ? "1" : "0") + " argc=" + std::to_string(a.size()));
        emit("{\"type\":\"agent_end\"}");
        return 0;
    }
    if (script == "echotoken") {
        emit("{\"type\":\"agent_start\"}");
        std::fputs(("leaking " + token + "\n").c_str(), stderr);
        std::fflush(stderr);
        emit_text_turn("the token is " + token);
        emit("{\"type\":\"agent_end\"}");
        return 0;
    }
    if (script.rfind("spawn:", 0) == 0) {
        spawn_sleeper(a[0], script.substr(6), 800);
        emit("{\"type\":\"agent_start\"}");
        sleep_ms(30000);
        return 0;
    }
    if (script == "exit3") {
        emit("{\"type\":\"agent_start\"}");
        return 3;
    }
    if (script == "noend") {
        emit("{\"type\":\"agent_start\"}");
        return 0;
    }
    std::fprintf(stderr, "unknown agent script: %s\n", script.c_str());
    return 4;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> a(argv, argv + argc);
    if (a.size() < 2) {
        std::fputs("usage: child_probe <cmd> ...\n", stderr);
        return 2;
    }
    const std::string cmd = a[1];
    if (cmd == "--offline") {
        return agent_sim(a);
    }

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

    if (cmd == "streams") {
        std::fputs("out1\n", stdout);
        std::fflush(stdout);
        std::fputs("err1\n", stderr);
        std::fflush(stderr);
        std::fputs("out2\n", stdout);
        std::fflush(stdout);
        return 0;
    }
    if (cmd == "envnames") {
#ifdef _WIN32
        if (LPCH block = GetEnvironmentStringsA()) {
            for (const char* cursor = block; *cursor != '\0';) {
                const std::string entry(cursor);
                cursor += entry.size() + 1;
                const std::size_t eq = entry.find('=');
                if (eq != std::string::npos && eq > 0) {
                    std::fputs((entry.substr(0, eq) + "\n").c_str(), stdout);
                }
            }
            FreeEnvironmentStringsA(block);
        }
#endif
        std::fflush(stdout);
        return 0;
    }
    if (cmd == "alloc") {
        const std::size_t mb = static_cast<std::size_t>(arg_int(a, 2, 64));
        try {
            std::vector<char> block(mb * 1024 * 1024, 1);
            volatile char sink = block[block.size() / 2];
            (void)sink;
            std::fputs("alloc-ok\n", stdout);
            std::fflush(stdout);
            sleep_ms(5000);
        } catch (const std::bad_alloc&) {
            std::fputs("alloc-failed\n", stdout);
            std::fflush(stdout);
            return 4;
        }
        return 0;
    }
    if (cmd == "fork") {
        const int n = arg_int(a, 2, 4);
        const int ms = arg_int(a, 3, 500);
        const std::string prefix = arg_str(a, 4);
        for (int i = 0; i < n; ++i) {
            spawn_sleeper(a[0], prefix + std::to_string(i), ms);
        }
        sleep_ms(ms + 1500);
        return 0;
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 3;
}
