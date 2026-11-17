#pragma once
// HCS coding-agent sidecar configuration (ADR-011).
//
// This module produces the exact documents and argv a supervisor needs to start the prime-agent sidecar;
// it never starts a process. Two invariants are enforced here so a caller cannot weaken them:
//   * the model provider must be loopback-only (ADR-011: no network by default), and
//   * the agent workspace must lie inside an approved root (PathGuard).
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"

namespace hcs::agent {

struct ProviderModel {
    std::string id;                             // the model id served by HCSInfer
    int context_window = 8192;                  // tokens advertised to the agent
    int max_tokens = 2048;                      // per-reply cap advertised to the agent
    std::vector<std::string> input = {"text"};  // capabilities advertised: "text" and/or "image"
};

struct ProviderConfig {
    std::string name = "hcs";  // identifier used with `--provider`
    std::string base_url;      // loopback only, e.g. http://127.0.0.1:8765/v1
    std::string api_key;       // the per-run loopback token; never logged
    std::string api = "openai-completions";
    bool auth_header = true;
    std::vector<ProviderModel> models;  // at least one
};

// True only for http://127.0.0.1[:port]/v1, http://localhost[:port]/v1 or http://[::1][:port]/v1.
bool is_loopback_base_url(std::string_view base_url);

// Validates the provider config. Returns InvalidArgument with a precise reason on any defect.
Result<void> validate_provider(const ProviderConfig& config);

// Validates and serialises the prime-agent `models.json` provider document.
Result<std::string> provider_config_json(const ProviderConfig& config);

struct LaunchRequest {
    std::filesystem::path program;    // prime-agent executable (absolute)
    PathGuard workspaces;             // approved roots
    std::filesystem::path workspace;  // must resolve inside `workspaces`
    std::filesystem::path agent_dir;  // PRIME_AGENT_CODING_AGENT_DIR (absolute)
    ProviderConfig provider;
    std::string model_id;  // must be one of provider.models
};

struct LaunchPlan {
    std::filesystem::path program;
    std::vector<std::string> args;           // argv, excluding argv[0]
    std::filesystem::path cwd;               // canonical workspace, inside an approved root
    std::map<std::string, std::string> env;  // PRIME_AGENT_CODING_AGENT_DIR, PI_OFFLINE
};

// Builds the launch plan (never runs it): `--offline`, only the HCS provider, the workspace as cwd.
Result<LaunchPlan> build_launch_plan(const LaunchRequest& request);

}  // namespace hcs::agent
