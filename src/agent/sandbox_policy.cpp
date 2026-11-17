#include "hcs/agent/sandbox_policy.hpp"

#include <algorithm>
#include <iterator>
#include <cctype>
#include <system_error>

namespace hcs::agent {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Returns the canonical program stem or nullopt when the name is not a plain, safe program name.
std::optional<std::string> canonical_program(std::string_view name) {
    if (name.empty() || name.size() > 64)
        return std::nullopt;
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-'))
            return std::nullopt;  // separators, ':', spaces, quotes, shell metacharacters, NUL
    }
    if (name.back() == '.')
        return std::nullopt;  // Windows strips trailing dots: "git." would run "git"
    std::string s = lower(name);
    for (const std::string_view script_ext : {".bat", ".cmd", ".com", ".ps1", ".vbs", ".js", ".lnk"}) {
        if (ends_with(s, script_ext))
            return std::nullopt;  // these are interpreted through a shell host
    }
    if (ends_with(s, ".exe"))
        s.resize(s.size() - 4);
    if (s.empty())
        return std::nullopt;
    return s;
}

bool has_dotdot_component(std::string_view s) {
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find_first_of("/\\", start);
        if (end == std::string_view::npos)
            end = s.size();
        if (s.substr(start, end - start) == "..")
            return true;
        start = end + 1;
    }
    return false;
}

bool looks_absolute(std::string_view s) {
    if (s.empty())
        return false;
    if (s[0] == '/' || s[0] == '\\')
        return true;
    return s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':';
}

bool has_url_scheme(std::string_view s) {
    return s.find("://") != std::string_view::npos;
}

bool arg_denied(const std::string& arg_lower, const std::vector<std::string>& denied) {
    for (const auto& d : denied) {
        const std::string dl = lower(d);
        if (arg_lower == dl || (arg_lower.size() > dl.size() && arg_lower.compare(0, dl.size(), dl) == 0 &&
                                arg_lower[dl.size()] == '='))
            return true;
    }
    return false;
}

bool is_git_dir_component(const fs::path::string_type& comp) {
    std::string s;
    s.reserve(comp.size());
    for (const auto ch : comp)
        s.push_back(static_cast<unsigned>(ch) < 128U ? static_cast<char>(std::tolower(static_cast<int>(ch)))
                                                     : '?');
    // ".git" and its NTFS 8.3 short name ("GIT~1"), which Windows resolves to the same directory.
    return s == ".git" || s.rfind("git~", 0) == 0;
}

}  // namespace

SandboxPolicy default_policy() {
    SandboxPolicy p;
    p.commands = {
        {"git",
         {"status", "diff", "log", "show", "rev-parse", "branch", "ls-files"},
         {"--output", "--ext-diff"}},
        {"rg", {}, {"--pre", "--pre-glob", "--hostname-bin"}},
        {"cmake", {}, {}},
        {"ctest", {}, {}},
        {"ninja", {}, {}},
    };
    return p;
}

bool is_hard_denied_program(std::string_view canonical_program_name) {
    static constexpr std::string_view kDenied[] = {
        // shells and script hosts
        "cmd",
        "powershell",
        "pwsh",
        "bash",
        "sh",
        "zsh",
        "fish",
        "wsl",
        "mshta",
        "wscript",
        "cscript",
        // interpreters (arbitrary code via -c / -e)
        "python",
        "python3",
        "py",
        "node",
        "perl",
        "ruby",
        "php",
        "lua",
        "deno",
        "bun",
        // network and remote access
        "curl",
        "wget",
        "ssh",
        "scp",
        "sftp",
        "ftp",
        "telnet",
        "nc",
        "ncat",
        "netcat",
        "certutil",
        "bitsadmin",
        // persistence, privilege and system change
        "runas",
        "schtasks",
        "sc",
        "reg",
        "rundll32",
        "regsvr32",
        "msiexec",
    };
    const std::string name = lower(canonical_program_name);
    return std::find(std::begin(kDenied), std::end(kDenied), std::string_view(name)) != std::end(kDenied);
}

Result<ToolSandbox> ToolSandbox::create(PathGuard guard, SandboxPolicy policy) {
    if (guard.empty())
        return make_error(ErrorCode::InvalidArgument, "sandbox needs at least one approved root");
    const auto& l = policy.limits;
    if (l.default_timeout_ms == 0 || l.max_timeout_ms < l.default_timeout_ms || l.max_output_bytes == 0 ||
        l.max_argc == 0 || l.max_arg_bytes == 0 || l.max_write_bytes == 0)
        return make_error(ErrorCode::InvalidArgument,
                          "sandbox limits must be non-zero and default <= maximum");
    std::vector<std::string> seen;
    for (const auto& rule : policy.commands) {
        const auto canon = canonical_program(rule.program);
        if (!canon || *canon != rule.program)
            return make_error(
                ErrorCode::InvalidArgument,
                "rule program must be a canonical lower-case bare name: '" + rule.program + "'");
        if (is_hard_denied_program(*canon))
            return make_error(ErrorCode::SecurityViolation,
                              "program can never be allow-listed: '" + rule.program + "'");
        if (std::find(seen.begin(), seen.end(), *canon) != seen.end())
            return make_error(ErrorCode::AlreadyExists, "duplicate rule for '" + rule.program + "'");
        seen.push_back(*canon);
    }
    return ToolSandbox(std::move(guard), std::move(policy));
}

Result<fs::path> ToolSandbox::authorize_read(const fs::path& path) const {
    if (path.empty() || !path.is_absolute())
        return make_error(ErrorCode::InvalidArgument, "tool paths must be absolute");
    return guard_.resolve(path);
}

Result<fs::path> ToolSandbox::authorize_write(const fs::path& path, std::size_t content_bytes) const {
    if (content_bytes > policy_.limits.max_write_bytes)
        return make_error(ErrorCode::InvalidArgument, "write exceeds the sandbox size cap");
    auto resolved = authorize_read(path);
    if (!resolved)
        return resolved;
    for (const auto& comp : resolved.value()) {
        if (is_git_dir_component(comp.native()))
            return make_error(ErrorCode::SecurityViolation, "writes inside a .git directory are not allowed");
    }
    std::error_code ec;
    if (fs::is_directory(resolved.value(), ec))
        return make_error(ErrorCode::InvalidArgument, "write target is a directory");
    return resolved;
}

Result<ApprovedCommand> ToolSandbox::authorize_command(const CommandRequest& req) const {
    const auto& lim = policy_.limits;
    if (req.argv.empty())
        return make_error(ErrorCode::InvalidArgument, "empty command");
    if (req.argv.size() > lim.max_argc)
        return make_error(ErrorCode::InvalidArgument, "too many arguments");
    for (const auto& a : req.argv) {
        if (a.size() > lim.max_arg_bytes)
            return make_error(ErrorCode::InvalidArgument, "argument too long");
        if (a.find('\0') != std::string::npos)
            return make_error(ErrorCode::InvalidArgument, "argument contains NUL");
    }

    const auto program = canonical_program(req.argv[0]);
    if (!program)
        return make_error(ErrorCode::SecurityViolation,
                          "program must be a bare name (no path, extension tricks or shell scripts)");
    if (is_hard_denied_program(*program))
        return make_error(ErrorCode::SecurityViolation, "program is denied: " + *program);
    const auto rule_it = std::find_if(policy_.commands.begin(), policy_.commands.end(),
                                      [&](const CommandRule& r) { return r.program == *program; });
    if (rule_it == policy_.commands.end())
        return make_error(ErrorCode::SecurityViolation, "program is not allow-listed: " + *program);
    const CommandRule& rule = *rule_it;

    if (!rule.subcommands.empty()) {
        if (req.argv.size() < 2 || std::find(rule.subcommands.begin(), rule.subcommands.end(), req.argv[1]) ==
                                       rule.subcommands.end())
            return make_error(ErrorCode::SecurityViolation,
                              "sub-command is not allow-listed for " + *program +
                                  " (it must be argv[1], options before it are not accepted)");
    }

    if (req.cwd.empty() || !req.cwd.is_absolute())
        return make_error(ErrorCode::InvalidArgument, "cwd must be absolute");
    auto cwd = guard_.resolve(req.cwd);
    if (!cwd)
        return cwd.error();
    std::error_code ec;
    if (!fs::is_directory(cwd.value(), ec))
        return make_error(ErrorCode::NotFound, "cwd is not an existing directory");

    for (std::size_t i = 1; i < req.argv.size(); ++i) {
        const std::string& arg = req.argv[i];
        if (arg_denied(lower(arg), rule.denied_args))
            return make_error(ErrorCode::SecurityViolation,
                              "argument is denied for " + *program + ": " + arg);
        // Check the argument itself and the value of `--flag=value`.
        std::vector<std::string_view> candidates{arg};
        if (!arg.empty() && arg[0] == '-') {
            const auto eq = arg.find('=');
            if (eq != std::string::npos)
                candidates.emplace_back(std::string_view(arg).substr(eq + 1));
        }
        for (const auto cand : candidates) {
            if (!policy_.allow_network && has_url_scheme(cand))
                return make_error(ErrorCode::SecurityViolation,
                                  "network access is disabled: URL argument rejected");
            if (looks_absolute(cand) || has_dotdot_component(cand)) {
                // PathGuard rejects '..', device prefixes and anything outside the approved roots.
                const fs::path p = looks_absolute(cand) ? fs::path(std::string(cand))
                                                        : cwd.value() / fs::path(std::string(cand));
                auto r = guard_.resolve(p);
                if (!r)
                    return make_error(r.error().code, "argument path is outside the approved roots: " + arg);
            }
        }
    }

    ApprovedCommand out;
    out.argv = req.argv;
    out.cwd = std::move(cwd).value();
    out.max_output_bytes = lim.max_output_bytes;
    if (!req.timeout_ms) {
        out.timeout_ms = lim.default_timeout_ms;
    } else if (*req.timeout_ms == 0) {
        return make_error(ErrorCode::InvalidArgument, "timeout must be greater than zero");
    } else if (*req.timeout_ms > lim.max_timeout_ms) {
        out.timeout_ms = lim.max_timeout_ms;
        out.timeout_clamped = true;
    } else {
        out.timeout_ms = *req.timeout_ms;
    }
    return out;
}

CappedOutput ToolSandbox::cap_output(std::string_view output) const {
    CappedOutput r;
    r.original_bytes = output.size();
    std::size_t keep = std::min(output.size(), policy_.limits.max_output_bytes);
    if (keep < output.size()) {
        r.truncated = true;
        while (keep > 0 && (static_cast<unsigned char>(output[keep]) & 0xC0U) == 0x80U)
            --keep;  // do not cut a UTF-8 sequence in half
    }
    r.text.assign(output.substr(0, keep));
    return r;
}

}  // namespace hcs::agent
