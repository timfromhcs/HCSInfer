#pragma once
// Tool-call audit for the HCS coding agent (ADR-011, "tripwire" tier).
//
// prime-agent's only model tool is `python_repl`: the model writes Python that the kernel runs with the
// user's permissions. HCS cannot put a policy check between the model and that kernel, so this layer inspects
// every tool call the moment the `tool_execution_start` event is seen and returns a verdict. The session
// manager terminates the whole process tree when the verdict is a violation.
//
// What this IS: a static, token-level scan of the submitted Python (strings and comments are lexed, not
// regex-matched, so `re.compile` or a docstring mentioning `eval(` does not trip it) that
//   - rejects tools other than `python_repl`;
//   - rejects dynamic code (`exec`, `eval`, `compile`, `__import__`, `importlib`, `ctypes`, ...) because it
//   cannot
//     be audited, and network/process-spawn modules and calls;
//   - extracts every literal shell command (`bash('...')`, `os.system`, `subprocess.*`) and checks it against
//   a
//     program allow-list (`ToolSandbox` + a Python rule that only permits `-m unittest|pytest|...` or a
//     script inside the root); non-literal commands are rejected;
//   - checks path literals (absolute paths, `..` components) against the approved root through `PathGuard`.
// What this is NOT: a sandbox. Code that is built at runtime from non-literal pieces, a race between the
// event and the tool starting, or an allow-listed build tool that runs project code can all do more than the
// policy intends. The Windows Job Object of the session limits the damage (process count, memory, UI) but
// does not confine file or network access. Treat the workspace as disposable and see docs/agent.md.
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/agent/sandbox_policy.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"

namespace hcs::agent {

struct AuditVerdict {
    bool allowed = true;
    std::string rule;  // machine readable: "ok", "unknown_tool", "network", "dynamic_code", "process_spawn",
                       // "command_not_allowed", "unauditable_command", "path_escape", "malformed"
    std::string detail;  // short human text (never contains the code itself)
};

struct AuditPolicy {
    // Modules that may follow `python -m` in a shell command.
    std::vector<std::string> python_modules = {"unittest", "pytest", "py_compile", "compileall", "doctest"};
    // Extra programs on top of default_policy() (git read-only, rg, cmake, ctest, ninja).
    std::vector<CommandRule> extra_commands;
    // Python module roots whose import is rejected (network, process, native code).
    std::vector<std::string> denied_imports;
    std::size_t max_code_bytes = std::size_t{64} * 1024;
};

// Conservative defaults for a small local coding agent: compilers/make for small C projects, plus a
// denied-import list covering sockets, HTTP clients, FFI and process APIs.
AuditPolicy default_audit_policy();

class ToolAudit {
public:
    // `root` must be the session's approved workspace (a directory that exists). Fails on a bad policy.
    static Result<ToolAudit> create(const std::filesystem::path& root,
                                    AuditPolicy policy = default_audit_policy());

    // `tool` is the prime-agent tool name; `payload` is its code (python_repl) text.
    AuditVerdict audit_tool_call(std::string_view tool, std::string_view payload) const;

    // Exposed for tests: audit one shell command line (no shell metacharacters are accepted).
    AuditVerdict audit_command_line(std::string_view command) const;
    AuditVerdict audit_argv(const std::vector<std::string>& argv) const;

    const std::filesystem::path& root() const noexcept { return root_; }

private:
    ToolAudit(std::filesystem::path root, ToolSandbox sandbox, AuditPolicy policy)
        : root_(std::move(root)), sandbox_(std::move(sandbox)), policy_(std::move(policy)) {}
    AuditVerdict audit_python(std::string_view code, int depth = 0) const;
    AuditVerdict audit_path_literal(std::string_view literal) const;

    std::filesystem::path root_;
    ToolSandbox sandbox_;
    AuditPolicy policy_;
};

}  // namespace hcs::agent
