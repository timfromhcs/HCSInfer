#pragma once
// MCP server over stdio (newline-delimited JSON-RPC). Dual-era:
//   * modern  (2026-07-28): stateless; every request carries _meta."io.modelcontextprotocol/protocolVersion";
//     server/discover is answered; no initialize handshake.
//   * legacy  (2025-11-25): initialize / notifications/initialized / ping.
// stdout carries ONLY JSON-RPC messages; all logging goes to the Logger (stderr).
#include <atomic>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "hcs/api/control_plane.hpp"

namespace hcs::mcp {

inline constexpr const char* kModernVersion = "2026-07-28";
inline constexpr const char* kLegacyVersion = "2025-11-25";

struct McpOptions {
    std::string server_name = "hcsinfer";
    std::string server_version = "unknown";
    std::size_t max_line_bytes = std::size_t{1024} * 1024;  // longer input lines are rejected (-32600)
    std::size_t max_concurrent_calls = 4;            // in-flight tools/call; more => error result "busy"
    std::chrono::milliseconds call_timeout{300000};  // hard cap per tool call
    std::size_t max_image_bytes = std::size_t{8} * 1024 * 1024;
};

// Tool schemas (also documented in docs/mcp.md).
nlohmann::json tool_list(std::uint32_t max_tokens_cap);

class McpServer {
public:
    McpServer(api::ControlPlane& cp, McpOptions options, Logger& logger);
    ~McpServer();

    // Reads until EOF, dispatching tools/call to worker threads; joins them before returning.
    // Output lines are written to `out` under a mutex.
    void run(std::istream& in, std::ostream& out);

    // Synchronous single-message entry point (tests / simple clients). Returns the response
    // line (without newline) or an empty string for notifications and cancelled requests.
    // tools/call is executed inline here.
    std::string handle_line(const std::string& line);

private:
    struct CallSlot;
    // Returns a JSON-RPC response object (or null json => nothing to send).
    nlohmann::json handle_message(const nlohmann::json& msg, const std::function<bool()>& cancelled_probe,
                                  const std::string& req_key);
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args, const std::string& req_key);
    nlohmann::json rpc_error(const nlohmann::json& id, int code, const std::string& msg,
                             nlohmann::json data = nullptr) const;
    nlohmann::json ok_result(const nlohmann::json& id, nlohmann::json result, bool modern) const;
    nlohmann::json tool_ok(nlohmann::json structured) const;
    nlohmann::json tool_err(const std::string& msg, const std::string& code) const;
    nlohmann::json tool_err(const Error& e) const;
    void cancel_request(const nlohmann::json& request_id);
    nlohmann::json wait_chat(const api::Submitted& sub, const std::string& req_key);
    nlohmann::json wait_image(const api::Submitted& sub, const std::string& req_key);
    nlohmann::json wait_benchmark(const JobId& id, const std::string& req_key);

    api::ControlPlane& cp_;
    McpOptions opt_;
    Logger& log_;
    nlohmann::json tools_;
    std::map<std::string, nlohmann::json> schemas_;

    std::mutex mu_;
    bool legacy_session_ = false;
    bool legacy_initialized_ = false;
    std::map<std::string, std::uint64_t> running_jobs_;  // request key -> job id
    std::map<std::string, bool> cancelled_;              // request key -> cancelled flag
    std::size_t active_calls_ = 0;
};

}  // namespace hcs::mcp

namespace hcs::mcp {

// `HCSInfer mcp stdio ...`: builds the runtime and serves JSON-RPC on `in`/`out`.
// Logs go to stderr only. Returns a process exit code.
int run_stdio(const std::vector<std::string>& args, const std::filesystem::path& state_dir, std::istream& in,
              std::ostream& out, std::ostream& err);

}  // namespace hcs::mcp
