#pragma once
// Minimal blocking HTTP client for the CLI commands that talk to a running `HCSInfer serve`
// (`models load|unload|status`). Plain HTTP to a host:port the user named (default 127.0.0.1:8765);
// the bearer token is only ever sent in the Authorization header and is never logged or echoed.
#include <string>
#include <string_view>

#include "hcs/result.hpp"

namespace hcs::api {

struct ClientTarget {
    std::string host = "127.0.0.1";
    int port = 8765;
    std::string token;  // empty => no Authorization header (only /hcs/v1/health works without one)
    int connect_timeout_s = 3;
    int read_timeout_s = 30;
};

struct ClientReply {
    int status = 0;
    std::string body;
};

// One request/response. Transport failure (nothing listening, reset, timeout) => Error(IoError / Timeout)
// whose message says no server answered; an HTTP error status is NOT an Error here (see error_from_reply).
hcs::Result<ClientReply> http_call(const ClientTarget& target, std::string_view method,
                                   const std::string& path, const std::string& body = {});

// Turns a non-2xx reply into the matching hcs Error: the server's {"error":{"code":..., "message":...}} is
// mapped back to its ErrorCode (so the CLI exit code equals what the API reported); 401 => SecurityViolation.
hcs::Error error_from_reply(const ClientReply& reply);

}  // namespace hcs::api
