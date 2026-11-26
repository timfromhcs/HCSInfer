#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace hcs {

// Environment lookup seam so the resolution order is unit-testable without touching the process environment.
// Returns the variable's value, or an empty string when it is unset or empty.
using EnvLookup = std::function<std::string(const char* name)>;

// Reads the real process environment (empty string when unset).
std::string process_env(const char* name);

// Resolves the directory that holds ALL HCS-owned state (settings.json, registry.json, logs, cache stats,
// sessions, train adapters, shader/kernel profile caches).
//
// Order (first non-empty wins):
//   1. `override_dir`        (--state-dir)
//   2. $HCS_DATA_DIR         (the "nothing on C:" switch: when set it is used verbatim and the per-user
//                             %LOCALAPPDATA% / $HOME defaults are NEVER consulted, not even as a fallback)
//   3. $HCS_STATE_DIR        (older name, kept for compatibility)
//   4. %LOCALAPPDATA%\HCSInfer
//   5. $HOME/.hcsinfer
//   6. ./.hcsinfer
std::filesystem::path resolve_state_dir(const std::string& override_dir, const EnvLookup& env);
std::filesystem::path resolve_state_dir(const std::string& override_dir);

}  // namespace hcs
