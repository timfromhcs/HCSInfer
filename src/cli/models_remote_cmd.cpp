#include "models_remote_cmd.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <ostream>
#include <thread>

#include "hcs/model_registry.hpp"
#include "hcs/result.hpp"

#ifdef HCS_HAVE_API
#include "hcs/api/client.hpp"
#endif

namespace hcs::cli {

using nlohmann::json;

const char* models_remote_help() {
    return "Usage: HCSInfer models load <id>   [connection options] [--timeout-s <n>] [--no-wait] [--json]\n"
           "       HCSInfer models unload [<id>] [connection options] [--json]\n"
           "       HCSInfer models status      [connection options] [--json]\n"
           "\n"
           "Talk to a RUNNING `HCSInfer serve` over its HTTP API (the model lives in that process).\n"
           "Connection options:\n"
           "  --host <addr>      Server address (default 127.0.0.1)\n"
           "  --port <n>         Server port (default 8765)\n"
           "  --token <secret>   API token the server printed (or set HCS_API_TOKEN; the env var keeps it "
           "out\n"
           "                     of the process list). `status` works without a token for the health check.\n"
           "load waits for the load job (default --timeout-s 900; --no-wait returns the job id at once).\n"
           "Exit codes are the API's: 0 ok, 7 unknown model, 5 out of memory, 3 model error, 11 nothing\n"
           "loaded / invalid state, 8 no server reachable, 9 bad token, 14 timeout.\n";
}

#ifndef HCS_HAVE_API

int run_models_remote(const std::string& sub, std::vector<std::string>, std::ostream&, std::ostream& err) {
    err << "error: 'models " << sub << "' needs the HTTP API module, which is not built into this binary"
        << " [unsupported]\n";
    return exit_code(ErrorCode::Unsupported);
}

#else

namespace {

int fail(std::ostream& err, const Error& e) {
    err << "error: " << e.message << " [" << to_string(e.code) << "]\n";
    return e.exit_code();
}
int fail(std::ostream& err, ErrorCode c, const std::string& m) {
    return fail(err, Error(c, m));
}

bool take_flag(std::vector<std::string>& v, const std::string& f) {
    auto it = std::find(v.begin(), v.end(), f);
    if (it == v.end())
        return false;
    v.erase(it);
    return true;
}

// 0 = absent, 1 = taken, -1 = present without a value.
int take_opt(std::vector<std::string>& v, const std::string& f, std::string& value) {
    auto it = std::find(v.begin(), v.end(), f);
    if (it == v.end())
        return 0;
    if (it + 1 == v.end())
        return -1;
    value = *(it + 1);
    v.erase(it, it + 2);
    return 1;
}

bool parse_uint(const std::string& s, unsigned long long max, unsigned long long& out) {
    if (s.empty() || s.size() > 9 || s.find_first_not_of("0123456789") != std::string::npos)
        return false;
    out = std::stoull(s);
    return out <= max;
}

struct Conn {
    api::ClientTarget target;
    bool as_json = false;
    bool no_wait = false;
    unsigned long long timeout_s = 900;
};

// Parses the shared options; returns an exit code >= 0 on error, -1 on success.
int parse_conn(std::vector<std::string>& args, Conn& c, std::ostream& err) {
    c.as_json = take_flag(args, "--json");
    c.no_wait = take_flag(args, "--no-wait");
    std::string v;
    int r = take_opt(args, "--host", v);
    if (r < 0)
        return fail(err, ErrorCode::InvalidArgument, "--host needs a value");
    if (r > 0)
        c.target.host = v;
    r = take_opt(args, "--port", v);
    unsigned long long n = 0;
    if (r < 0 || (r > 0 && (!parse_uint(v, 65535, n) || n == 0)))
        return fail(err, ErrorCode::InvalidArgument, "--port must be 1..65535");
    if (r > 0)
        c.target.port = static_cast<int>(n);
    r = take_opt(args, "--timeout-s", v);
    if (r < 0 || (r > 0 && (!parse_uint(v, 86400, n) || n == 0)))
        return fail(err, ErrorCode::InvalidArgument, "--timeout-s must be 1..86400");
    if (r > 0)
        c.timeout_s = n;
    r = take_opt(args, "--token", v);
    if (r < 0)
        return fail(err, ErrorCode::InvalidArgument, "--token needs a value");
    if (r > 0)
        c.target.token = v;
    else if (const char* e = std::getenv("HCS_API_TOKEN"); e && *e)
        c.target.token = e;
    return -1;
}

// Calls the API; HTTP errors become hcs Errors.
Result<json> call_json(const Conn& c, const char* method, const std::string& path,
                       const std::string& body = {}) {
    auto r = api::http_call(c.target, method, path, body);
    if (!r)
        return r.error();
    if (r.value().status < 200 || r.value().status > 299)
        return api::error_from_reply(r.value());
    auto j = json::parse(r.value().body, nullptr, false);
    if (j.is_discarded())
        return Error(ErrorCode::Internal, "server sent a reply that is not JSON");
    return j;
}

int need_token(const Conn& c, std::ostream& err) {
    if (!c.target.token.empty())
        return -1;
    return fail(err, ErrorCode::InvalidArgument,
                "an API token is required: pass --token <secret> or set HCS_API_TOKEN to the token `HCSInfer "
                "serve` printed");
}

int cmd_load(Conn& c, const std::string& id, std::ostream& out, std::ostream& err) {
    if (auto rc = need_token(c, err); rc >= 0)
        return rc;
    auto start = call_json(c, "POST", "/hcs/v1/models/" + id + "/load", "{}");
    if (!start)
        return fail(err, start.error());
    const std::string job = start.value().value("job_id", "");
    if (job.size() < 5 || job.size() > 24 || job.compare(0, 4, "job-") != 0 ||
        job.find_first_not_of("0123456789", 4) != std::string::npos)
        return fail(err, ErrorCode::Internal, "server did not return a usable job id");
    if (c.no_wait) {
        if (c.as_json)
            out << json{{"id", id}, {"job_id", job}, {"state", "queued"}}.dump(2) << "\n";
        else
            out << "load of " << id << " started as job " << job << "\n";
        return 0;
    }
    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + std::chrono::seconds(c.timeout_s);
    std::string state;
    json last;
    for (;;) {
        auto st = call_json(c, "GET", "/hcs/v1/jobs/" + job);
        if (!st)
            return fail(err, st.error());
        last = st.value();
        state = last.value("state", "");
        if (state == "succeeded" || state == "failed" || state == "cancelled")
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            err << "error: load of " << id << " is still " << state << " after " << c.timeout_s << " s (job "
                << job << " keeps running; check `HCSInfer models status`) [timeout]\n";
            return exit_code(ErrorCode::Timeout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    if (state == "succeeded") {
        if (c.as_json)
            out << json{{"id", id}, {"job_id", job}, {"state", "loaded"}, {"wall_s", secs}}.dump(2) << "\n";
        else
            out << "loaded " << id << " in " << static_cast<int>(secs * 10 + 0.5) / 10.0 << " s\n";
        return 0;
    }
    if (state == "cancelled")
        return fail(err, ErrorCode::Cancelled, "load of " + id + " was cancelled");
    ErrorCode code = ErrorCode::Internal;
    std::string msg = "load failed";
    if (last.contains("error") && last["error"].is_object()) {
        msg = last["error"].value("message", msg);
        const std::string name = last["error"].value("code", "");
        for (int v = 1; v <= 70; ++v)
            if (to_string(static_cast<ErrorCode>(v)) == name)
                code = static_cast<ErrorCode>(v);
    }
    return fail(err, Error(code, msg));
}

int cmd_unload(Conn& c, std::string id, std::ostream& out, std::ostream& err) {
    if (auto rc = need_token(c, err); rc >= 0)
        return rc;
    if (id.empty()) {
        auto rt = call_json(c, "GET", "/hcs/v1/runtime");
        if (!rt)
            return fail(err, rt.error());
        if (!rt.value().contains("loaded_model") || !rt.value()["loaded_model"].is_string())
            return fail(err, ErrorCode::InvalidState, "no model is loaded");
        id = rt.value()["loaded_model"].get<std::string>();
    }
    auto r = call_json(c, "POST", "/hcs/v1/models/" + id + "/unload", "{}");
    if (!r)
        return fail(err, r.error());
    if (c.as_json)
        out << json{{"id", id}, {"state", "unloaded"}}.dump(2) << "\n";
    else
        out << "unloaded " << id << "\n";
    return 0;
}

int cmd_status(Conn& c, std::ostream& out, std::ostream& err) {
    auto health = call_json(c, "GET", "/hcs/v1/health");
    if (!health)
        return fail(err, health.error());
    json j;
    j["server"] = {{"reachable", true},
                   {"host", c.target.host},
                   {"port", c.target.port},
                   {"health", health.value().value("status", "unknown")}};
    if (c.target.token.empty()) {
        j["note"] = "no API token given: only the health check was queried (pass --token or HCS_API_TOKEN)";
    } else {
        auto rt = call_json(c, "GET", "/hcs/v1/runtime");
        if (!rt)
            return fail(err, rt.error());
        j["loaded_model"] = rt.value().value("loaded_model", json(nullptr));
        j["jobs_pending"] = rt.value().value("jobs_pending", 0);
        j["uptime_s"] = rt.value().value("uptime_s", 0.0);
        j["version"] = rt.value().value("version", "unknown");
        if (rt.value().contains("vault"))
            j["vault"] = rt.value()["vault"];
        if (auto mem = call_json(c, "GET", "/hcs/v1/memory"); mem) {
            j["memory"] = {{"budget_known", mem.value().value("budget_known", false)},
                           {"reserved_bytes", mem.value().value("reserved_bytes", 0)}};
        }
    }
    if (c.as_json) {
        out << j.dump(2) << "\n";
        return 0;
    }
    out << "server:       " << c.target.host << ":" << c.target.port << " ("
        << j["server"]["health"].get<std::string>() << ")\n";
    if (j.contains("note")) {
        out << j["note"].get<std::string>() << "\n";
        return 0;
    }
    out << "version:      " << j["version"].get<std::string>() << "\n";
    out << "loaded model: " << (j["loaded_model"].is_string() ? j["loaded_model"].get<std::string>() : "none")
        << "\n";
    out << "jobs pending: " << j["jobs_pending"] << "\n";
    if (j.contains("vault"))
        out << "vault load:   " << j["vault"].dump() << "\n";
    return 0;
}

}  // namespace

int run_models_remote(const std::string& sub, std::vector<std::string> args, std::ostream& out,
                      std::ostream& err) {
    if (take_flag(args, "--help") || take_flag(args, "-h")) {
        out << models_remote_help();
        return 0;
    }
    Conn c;
    if (int rc = parse_conn(args, c, err); rc >= 0)
        return rc;
    if (sub == "status") {
        if (!args.empty())
            return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
        return cmd_status(c, out, err);
    }
    if (sub == "unload" && args.size() > 1)
        return fail(err, ErrorCode::InvalidArgument, "unload takes at most one <id>");
    if (sub == "load" && args.size() != 1)
        return fail(err, ErrorCode::InvalidArgument, "load requires exactly one <id>");
    if (!args.empty() && !is_valid_model_id(args[0]))
        return fail(err, ErrorCode::InvalidArgument, "invalid model id");
    if (sub == "load")
        return cmd_load(c, args[0], out, err);
    return cmd_unload(c, args.empty() ? std::string() : args[0], out, err);
}

#endif  // HCS_HAVE_API

}  // namespace hcs::cli
