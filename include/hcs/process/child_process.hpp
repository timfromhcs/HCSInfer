#pragma once
// Child-process supervision (ADR-011 items 1 and 6): start a helper program, capture its merged output, wait
// with a timeout, and guarantee that the WHOLE process tree dies with this object.
//
// No shell is involved: argv is handed to CreateProcessW directly, so quotes/redirection/metacharacters are
// never interpreted. On Windows the process is placed in a Job Object with kill-on-close, so a crash of
// HCSInfer also reaps the tree. Non-Windows builds return Unsupported (the agent ships for Windows first).
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hcs/result.hpp"

namespace hcs::process {

struct SpawnOptions {
    std::filesystem::path cwd;                                // absolute working directory
    std::vector<std::string> args;                            // argv[1..]; never parsed by a shell
    std::map<std::string, std::string> env;                   // overrides merged into the parent environment
    bool capture_output = true;                               // capture merged stdout + stderr
    std::uint64_t max_output_bytes = std::uint64_t{1} << 20;  // cap on the captured output
};

enum class WaitState { Exited, TimedOut };

struct WaitResult {
    WaitState state = WaitState::TimedOut;
    int exit_code = -1;  // valid only when state == Exited
    std::uint32_t pid = 0;
};

class ChildProcess {
public:
    ChildProcess() noexcept;
    ~ChildProcess();
    ChildProcess(ChildProcess&&) noexcept;
    ChildProcess& operator=(ChildProcess&&) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts `program` (absolute path to a file). On success the process is already inside a kill-on-close
    // job object, so destroying this object kills the tree.
    static Result<ChildProcess> spawn(const std::filesystem::path& program, const SpawnOptions& options);

    bool running() const;
    std::uint32_t pid() const noexcept;

    // Waits up to `timeout_ms` (0 = poll). On Exited the captured output is complete and the exit code is
    // recorded. Reaping is idempotent.
    Result<WaitResult> wait_for(std::uint64_t timeout_ms);

    // Kills the whole tree (job object) and waits briefly for it to die.
    Result<void> terminate();

    // Merged stdout + stderr captured so far, truncated at the configured cap.
    std::string output() const;
    bool output_truncated() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit ChildProcess(std::unique_ptr<Impl> impl) noexcept;
};

}  // namespace hcs::process
