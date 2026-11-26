// cpp-httplib is third-party code confined to this file and http_server.cpp (relaxed warnings, see CMake).
#include <httplib.h>

#include <algorithm>
#include <nlohmann/json.hpp>

#include "hcs/api/client.hpp"

namespace hcs::api {

hcs::Result<ClientReply> http_call(const ClientTarget& t, std::string_view method, const std::string& path,
                                   const std::string& body) {
    if (method != "GET" && method != "POST")
        return Error(ErrorCode::InvalidArgument, "unsupported client method");
    if (t.host.empty() || t.port < 1 || t.port > 65535)
        return Error(ErrorCode::InvalidArgument, "invalid server address");
    httplib::Client c(t.host, t.port);
    // Windows retries a refused loopback connect for ~2 s before it reports failure; a loopback server that is
    // up accepts at once (backlog), so one second is enough to tell "nobody is listening" quickly.
    const bool loopback = t.host == "127.0.0.1" || t.host == "localhost" || t.host == "::1";
    c.set_connection_timeout(loopback ? std::min(t.connect_timeout_s, 1) : t.connect_timeout_s, 0);
    c.set_read_timeout(t.read_timeout_s, 0);
    c.set_write_timeout(t.connect_timeout_s + 5, 0);
    c.set_follow_location(false);
    httplib::Headers h;
    if (!t.token.empty())
        h.emplace("Authorization", "Bearer " + t.token);
    httplib::Result r = method == "GET" ? c.Get(path, h) : c.Post(path, h, body, "application/json");
    if (!r) {
        const std::string where = t.host + ":" + std::to_string(t.port);
        if (r.error() == httplib::Error::Read)
            return Error(ErrorCode::Timeout, "server at " + where + " did not answer in time");
        return Error(ErrorCode::IoError, "no HCSInfer server reachable at " + where +
                                             " (start one with `HCSInfer serve`, or pass --host/--port)");
    }
    ClientReply out;
    out.status = r->status;
    out.body = r->body;
    return out;
}

hcs::Error error_from_reply(const ClientReply& reply) {
    if (reply.status == 401)
        return Error(
            ErrorCode::SecurityViolation,
            "the server rejected the API token (pass --token or set HCS_API_TOKEN to the token `serve` "
            "printed)");
    std::string message = "server answered HTTP " + std::to_string(reply.status);
    ErrorCode code = ErrorCode::Internal;
    auto j = nlohmann::json::parse(reply.body, nullptr, false);
    if (j.is_object() && j.contains("error") && j["error"].is_object()) {
        const auto& e = j["error"];
        if (e.contains("message") && e["message"].is_string())
            message = e["message"].get<std::string>();
        if (e.contains("code") && e["code"].is_string()) {
            const std::string name = e["code"].get<std::string>();
            for (int v = 0; v <= 70; ++v) {
                const auto ec = static_cast<ErrorCode>(v);
                if (to_string(ec) == name && ec != ErrorCode::Ok) {
                    code = ec;
                    break;
                }
            }
            if (code == ErrorCode::Internal && name != "internal")
                message += " [" + name + "]";
        }
    } else if (reply.status == 403) {
        code = ErrorCode::SecurityViolation;
    }
    return Error(code, message);
}

}  // namespace hcs::api
