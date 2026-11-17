#pragma once
// Tool-call sandbox policy for the HCS coding agent (ADR-011).
//
// This is a POLICY layer, not an OS sandbox: it decides whether a requested tool call may run and returns
// the exact, already-validated values the executor must use. It never starts a process or opens a file.
// Build tools that are allowed (cmake, ninja, ...) still execute project code with the user's permissions;
// run untrusted repositories in an external sandbox (ADR-011 "Residual risk").
//
// Rules (all fail closed):
//  - commands are argv vectors; there is no shell string and no shell program can ever be allow-listed;
//  - argv[0] is a bare program name that must match an allow-list rule; the rule may pin the sub-command
//    (argv[1]) and list always-denied arguments;
//  - network is off by default: URL-looking arguments and network programs are rejected;
//  - every absolute path (also `--flag=<path>`), every `..` path and the working directory must resolve
//  inside
//    an approved root through hcs::PathGuard;
//  - the effective timeout and the captured-output size are bounded.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"

namespace hcs::agent {

struct CommandRule {
    std::string program;                   // canonical: lower case, no extension, e.g. "git"
    std::vector<std::string> subcommands;  // allowed argv[1]; empty = any (argv[1] is then not constrained)
    std::vector<std::string> denied_args;  // exact (case-insensitive) args, also matched as "<arg>=value"
};

struct SandboxLimits {
    std::uint32_t default_timeout_ms = 60'000;
    std::uint32_t max_timeout_ms = 600'000;
    std::size_t max_output_bytes = std::size_t{64} * 1024;
    std::size_t max_argc = 64;
    std::size_t max_arg_bytes = std::size_t{8} * 1024;
    std::size_t max_write_bytes = std::size_t{1024} * 1024;
};

struct SandboxPolicy {
    std::vector<CommandRule> commands;
    SandboxLimits limits;
    bool allow_network = false;  // V1: no code path sets this; kept so the default is explicit and testable
};

// Conservative defaults: read-only git, ripgrep, cmake, ctest, ninja.
SandboxPolicy default_policy();

// Programs that can never be allow-listed (shells, interpreters, network and persistence tools).
bool is_hard_denied_program(std::string_view canonical_program);

struct CommandRequest {
    std::vector<std::string> argv;
    std::filesystem::path cwd;  // absolute
    std::optional<std::uint32_t> timeout_ms;
};

struct ApprovedCommand {
    std::vector<std::string> argv;  // argv[0] unchanged; the executor resolves it, never via a shell
    std::filesystem::path cwd;      // canonical, inside an approved root
    std::uint32_t timeout_ms = 0;   // effective timeout
    bool timeout_clamped = false;   // request exceeded the maximum and was reduced
    std::size_t max_output_bytes = 0;
};

struct CappedOutput {
    std::string text;  // at most max_output_bytes, cut on a UTF-8 boundary
    bool truncated = false;
    std::size_t original_bytes = 0;
};

class ToolSandbox {
public:
    // Rejects a policy that allow-lists a hard-denied program, a duplicate rule or zero limits.
    static Result<ToolSandbox> create(PathGuard guard, SandboxPolicy policy);

    // Paths must be absolute. Returns the canonical path inside an approved root.
    Result<std::filesystem::path> authorize_read(const std::filesystem::path& path) const;
    // As authorize_read, plus: size cap, never inside a `.git` directory, never an existing directory.
    Result<std::filesystem::path> authorize_write(const std::filesystem::path& path,
                                                  std::size_t content_bytes) const;
    Result<ApprovedCommand> authorize_command(const CommandRequest& request) const;
    CappedOutput cap_output(std::string_view output) const;

    const SandboxPolicy& policy() const noexcept { return policy_; }

private:
    ToolSandbox(PathGuard guard, SandboxPolicy policy)
        : guard_(std::move(guard)), policy_(std::move(policy)) {}
    PathGuard guard_;
    SandboxPolicy policy_;
};

}  // namespace hcs::agent
