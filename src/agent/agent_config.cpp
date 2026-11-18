#include "hcs/agent/agent_config.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include <nlohmann/json.hpp>

namespace hcs::agent {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

bool is_control(char c) {
    return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f;
}

bool has_control_or_space(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) {
        return is_control(c) || std::isspace(static_cast<unsigned char>(c)) != 0;
    });
}

bool is_identifier(std::string_view s) {
    if (s.empty() || s.size() > 32) {
        return false;
    }
    return std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
}

bool is_model_id(std::string_view s) {
    if (s.empty() || s.size() > 128) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ':';
    });
}

bool is_env_name(std::string_view s) {
    if (s.empty() || s.size() > 64 || !(std::isupper(static_cast<unsigned char>(s.front())) != 0)) {
        return false;
    }
    return std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isupper(c) || std::isdigit(c) || c == '_'; });
}

bool is_capability(std::string_view s) {
    return s == "text" || s == "image";
}

// Returns the host part and the path of an `http://host[:port]/path` URL, or false when it is not that shape.
bool split_http_url(std::string_view url, std::string& host, std::string& port, std::string& path) {
    constexpr std::string_view scheme = "http://";
    if (url.size() <= scheme.size() || url.substr(0, scheme.size()) != scheme) {
        return false;
    }
    std::string_view rest = url.substr(scheme.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    std::string_view authority = rest.substr(0, slash);
    path = std::string(rest.substr(slash));
    if (authority.find('@') != std::string_view::npos) {
        return false;  // userinfo is never expected
    }
    if (authority.empty()) {
        return false;
    }
    if (authority.front() == '[') {  // IPv6 literal: [::1]
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        host = std::string(authority.substr(0, close + 1));
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                return false;
            }
            port = std::string(after.substr(1));
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            host = std::string(authority);
        } else {
            host = std::string(authority.substr(0, colon));
            port = std::string(authority.substr(colon + 1));
        }
    }
    return true;
}

Result<void> invalid(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

Result<LaunchPlan> invalid_plan(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

}  // namespace

bool is_loopback_base_url(std::string_view base_url) {
    std::string host;
    std::string port;
    std::string path;
    if (!split_http_url(base_url, host, port, path)) {
        return false;
    }
    if (host != "127.0.0.1" && host != "localhost" && host != "[::1]") {
        return false;
    }
    if (!port.empty()) {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        const unsigned long value = std::stoul(port);
        if (value == 0 || value > 65535) {
            return false;
        }
    }
    if (path != "/v1" && path != "/v1/") {
        return false;
    }
    if (base_url.find('?') != std::string_view::npos || base_url.find('#') != std::string_view::npos) {
        return false;
    }
    return true;
}

Result<void> validate_provider(const ProviderConfig& config) {
    if (!is_identifier(config.name)) {
        return invalid("provider name must be a short identifier ([A-Za-z0-9._-], max 32)");
    }
    if (!is_loopback_base_url(config.base_url)) {
        return invalid("provider base_url must be loopback (http://127.0.0.1[:port]/v1, localhost or [::1])");
    }
    if (config.api_key.empty() || config.api_key.size() > 512 || has_control_or_space(config.api_key)) {
        return invalid("provider api_key must be a non-empty token without whitespace or control characters");
    }
    if (!config.api_key_env.empty() && !is_env_name(config.api_key_env)) {
        return invalid(
            "provider api_key_env must be an upper-case environment variable name ([A-Z][A-Z0-9_]*)");
    }
    if (config.api != "openai-completions") {
        return invalid("provider api must be 'openai-completions'");
    }
    if (config.models.empty()) {
        return invalid("provider must list at least one model");
    }
    for (const auto& model : config.models) {
        if (!is_model_id(model.id)) {
            return invalid("model id '" + model.id +
                           "' is not a plain identifier ([A-Za-z0-9._:-], max 128)");
        }
        if (model.context_window < 128 || model.context_window > (1 << 20)) {
            return invalid("model '" + model.id + "' context_window must be between 128 and 1048576");
        }
        if (model.max_tokens < 1 || model.max_tokens > model.context_window) {
            return invalid("model '" + model.id + "' max_tokens must be between 1 and its context_window");
        }
        if (model.input.empty()) {
            return invalid("model '" + model.id + "' must advertise at least one input capability");
        }
        for (const auto& capability : model.input) {
            if (!is_capability(capability)) {
                return invalid("model '" + model.id + "' input capability '" + capability +
                               "' is not supported (text, image)");
            }
        }
    }
    return {};
}

Result<std::string> provider_config_json(const ProviderConfig& config) {
    if (auto v = validate_provider(config); !v) {
        return v.error();
    }
    json models = json::array();
    for (const auto& model : config.models) {
        models.push_back(json{{"id", model.id},
                              {"contextWindow", model.context_window},
                              {"maxTokens", model.max_tokens},
                              {"input", model.input}});
    }
    json provider{{"baseUrl", config.base_url},
                  {"apiKey", config.api_key_env.empty() ? config.api_key : config.api_key_env},
                  {"api", config.api},
                  {"authHeader", config.auth_header}};
    if (config.local_server_compat) {
        provider["compat"] = json{{"supportsStore", false},
                                  {"supportsDeveloperRole", false},
                                  {"supportsReasoningEffort", false},
                                  {"supportsUsageInStreaming", true},
                                  {"maxTokensField", "max_tokens"}};
    }
    provider["models"] = std::move(models);
    const json document{{"providers", json{{config.name, std::move(provider)}}}};
    return document.dump(2);
}

Result<LaunchPlan> build_launch_plan(const LaunchRequest& request) {
    if (!request.program.is_absolute() || request.program.filename().empty()) {
        return invalid_plan("agent program must be an absolute path to a file");
    }
    if (request.agent_dir.empty() || !request.agent_dir.is_absolute()) {
        return invalid_plan("agent_dir must be an absolute path");
    }
    if (request.workspaces.empty()) {
        return invalid_plan("at least one approved workspace root is required");
    }
    auto workspace = request.workspaces.resolve(request.workspace);
    if (!workspace) {
        return workspace.error();
    }
    if (auto v = validate_provider(request.provider); !v) {
        return v.error();
    }
    const auto model = std::find_if(request.provider.models.begin(), request.provider.models.end(),
                                    [&](const ProviderModel& m) { return m.id == request.model_id; });
    if (model == request.provider.models.end()) {
        return invalid_plan("model_id '" + request.model_id + "' is not one of the provider's models");
    }

    LaunchPlan plan;
    plan.program = request.program;
    plan.cwd = workspace.value();
    plan.args = {"--offline", "--provider", request.provider.name, "--model", request.model_id};
    plan.env = {{"PRIME_AGENT_CODING_AGENT_DIR", request.agent_dir.string()}, {"PI_OFFLINE", "1"}};
    if (request.provider.api_key_env.empty()) {
        plan.args.push_back("--api-key");
        plan.args.push_back(request.provider.api_key);
    } else {
        plan.env[request.provider.api_key_env] = request.provider.api_key;
    }
    return plan;
}

}  // namespace hcs::agent
