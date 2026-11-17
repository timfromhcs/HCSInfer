#include <iostream>

#include "hcs/api/runtime.hpp"
#include "hcs/mcp/mcp_server.hpp"
#include "hcs/version.hpp"

namespace hcs::mcp {

int run_stdio(const std::vector<std::string>& args, const std::filesystem::path& state_dir, std::istream& in,
              std::ostream& out, std::ostream& err) {
    std::vector<std::string> roots;
    McpOptions mo;
    mo.server_version = HCS_VERSION_STRING;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") {
            out << "Usage: HCSInfer mcp stdio [--model-root <dir>]... [--timeout-s <n>] [--max-concurrent <n>]\n"
                   "\n"
                   "Serves MCP (2026-07-28, plus the 2025-11-25 initialize handshake) as newline-delimited\n"
                   "JSON-RPC on stdin/stdout. Logs go to stderr. Model files must lie inside an approved root\n"
                   "(--model-root or settings.json). Closing stdin shuts the server down.\n";
            return 0;
        }
        auto need = [&](std::string& dst) {
            if (i + 1 >= args.size()) return false;
            dst = args[++i];
            return true;
        };
        std::string v;
        if (a == "--model-root") {
            if (!need(v)) { err << "error: --model-root needs a value [invalid_argument]\n"; return exit_code(ErrorCode::InvalidArgument); }
            roots.push_back(v);
        } else if (a == "--timeout-s" || a == "--max-concurrent") {
            if (!need(v) || v.empty() || v.size() > 6 || v.find_first_not_of("0123456789") != std::string::npos || std::stoul(v) == 0) {
                err << "error: " << a << " needs a positive integer [invalid_argument]\n";
                return exit_code(ErrorCode::InvalidArgument);
            }
            if (a == "--timeout-s") mo.call_timeout = std::chrono::seconds(std::stoul(v));
            else mo.max_concurrent_calls = std::stoul(v);
        } else {
            err << "error: unknown option: " << a << " [invalid_argument]\n";
            return exit_code(ErrorCode::InvalidArgument);
        }
    }
    Logger& log = global_logger();  // stderr
    api::RuntimeOptions ro;
    ro.state_dir = state_dir;
    ro.model_roots = roots;
    auto rt = api::Runtime::create(std::move(ro), log);
    if (!rt) {
        err << "error: " << rt.error().message << " [" << to_string(rt.error().code) << "]\n";
        return rt.error().exit_code();
    }
    McpServer server(*rt.value()->control, mo, log);
    log.info("mcp", "stdio server started", {{"protocol", kModernVersion}});
    server.run(in, out);
    rt.value()->queue->shutdown();
    rt.value()->engine_queue->shutdown();
    return 0;
}

}  // namespace hcs::mcp
