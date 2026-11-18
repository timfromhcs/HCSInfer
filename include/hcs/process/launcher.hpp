#pragma once
// Small launcher seam over hcs::process::ChildProcess (ADR-011): the agent session manager talks to an
// IProcessLauncher so tests can substitute a scripted fake, while production uses the one Job-Object
// implementation in child_process.cpp (there is deliberately no second launcher).
//
// A launched child always runs with: a COMPLETE replaced environment (nothing inherited), stdin = NUL,
// separate stdout / stderr pipes, a kill-on-close Job Object with the requested process-count and memory
// caps, and no UI access.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hcs/result.hpp"

namespace hcs::process {

struct LaunchSpec {
    std::filesystem::path exe;                // absolute
    std::vector<std::string> args;            // argv[1..]; quoted by the launcher, never via a shell
    std::filesystem::path cwd;                // absolute
    std::map<std::string, std::string> env;   // the COMPLETE child environment
    std::uint32_t active_process_limit = 32;  // live processes allowed in the tree
    std::uint64_t memory_limit_bytes = 0;     // committed memory of the tree; 0 = no cap
};

class IChildProcess {
public:
    virtual ~IChildProcess() = default;
    virtual std::uint32_t pid() const = 0;
    // Blocking reads; 0 means end of stream (the whole tree is gone or the pipe is closed).
    virtual std::size_t read_stdout(char* buffer, std::size_t capacity) = 0;
    virtual std::size_t read_stderr(char* buffer, std::size_t capacity) = 0;
    // Kills the whole process tree. Idempotent, callable from any thread.
    virtual void terminate() = 0;
    // Exit code once the main process has exited; nullopt when it is still running after `timeout_ms`.
    virtual std::optional<int> wait(std::uint32_t timeout_ms) = 0;
};

class IProcessLauncher {
public:
    virtual ~IProcessLauncher() = default;
    virtual Result<std::unique_ptr<IChildProcess>> launch(const LaunchSpec& spec) = 0;
};

// The ChildProcess-backed launcher. Non-Windows builds return Unsupported from launch() (NOT RUN there).
std::unique_ptr<IProcessLauncher> make_native_launcher();

}  // namespace hcs::process
