#include "hcs/api/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

#include "hcs/api/http_api.hpp"
#include "hcs/fs_util.hpp"
#include "hcs/version.hpp"

#ifdef HCS_HAVE_HARDWARE
#include "hcs/hardware/doctor.hpp"
#include "hcs/hardware/memory_probe.hpp"
#endif

namespace hcs::api {

#ifndef HCS_HAVE_LLAMA_ENGINE
std::unique_ptr<ITextEngine> make_default_text_engine(JobQueue&) {
    return nullptr;
}
#endif
#ifndef HCS_HAVE_DIFFUSION_ENGINE
std::unique_ptr<IImageEngine> make_default_image_engine(JobQueue&) {
    return nullptr;
}
#endif

Result<std::unique_ptr<Runtime>> Runtime::create(RuntimeOptions options, Logger& logger) {
    std::unique_ptr<Runtime> rt(new Runtime());
    const auto settings_file = options.state_dir / "settings.json";
    auto st = load_settings(settings_file);
    if (!st)
        return st.error();
    rt->settings = st.value();

    std::vector<std::string> roots = rt->settings.model_roots;
    for (auto& r : options.model_roots) roots.push_back(r);
    for (const auto& r : roots) {
        if (auto a = rt->guard.add_root(r); !a) {
            logger.warn("runtime", "ignoring approved root", {{"reason", a.error().message}});
        }
    }
    const auto reg_file = options.state_dir / "registry.json";
    std::error_code ec;
    if (std::filesystem::exists(reg_file, ec)) {
        auto text = read_file_limited(reg_file, 64u << 20);
        if (!text)
            return text.error();
        if (auto r = rt->registry.load_json(text.value()); !r)
            return r.error();
    }

    rt->queue = std::make_unique<JobQueue>(options.queue);
    JobQueueOptions eq;
    eq.workers = 1;
    rt->engine_queue = std::make_unique<JobQueue>(eq);
    rt->engine = options.engine ? std::move(options.engine) : make_default_text_engine(*rt->engine_queue);
    rt->image_engine =
        options.image_engine ? std::move(options.image_engine) : make_default_image_engine(*rt->engine_queue);

    ControlPlaneOptions co = options.control;
    co.version = HCS_VERSION_STRING;
    co.context_tokens = rt->settings.context_tokens;
    switch (rt->settings.accel) {
        case AccelProfile::CpuOnly:
            co.backend = Backend::Cpu;
            break;
        case AccelProfile::Vulkan:
            co.backend = Backend::Vulkan;
            break;
        case AccelProfile::Auto:
            co.backend = Backend::Auto;
            break;
    }
    // Live memory budget (AGENTS.md 10.1, M10 audit D4): an injected probe wins, else the OS measurement from
    // the hardware module. Without either, the budget stays unknown and /hcs/v1/memory says so.
    if (options.memory_probe)
        co.memory_probe = options.memory_probe;
#ifdef HCS_HAVE_HARDWARE
    if (!co.memory_probe)
        co.memory_probe = [] { return hcs::hardware::measure_memory(); };
#endif
#ifdef HCS_HAVE_HARDWARE
    if (!co.devices_provider) {
        co.devices_provider = [] {
            std::ostringstream ss;
            const int rc = hcs::hardware::run_doctor(true, ss);
            auto j = nlohmann::json::parse(ss.str(), nullptr, false);
            if (j.is_discarded() || rc != 0)
                return nlohmann::json{{"status", "unknown"}, {"reason", "doctor report unavailable"}};
            return j;
        };
    }
#endif
    rt->control = std::make_unique<ControlPlane>(rt->registry, rt->guard, rt->budget, *rt->queue,
                                                 rt->engine.get(), logger, co, rt->image_engine.get());
    return rt;
}

namespace {
std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) {
    g_stop.store(true);
}

const char* kServeHelp =
    "Usage: HCSInfer serve [options]\n"
    "\n"
    "Starts the local HTTP API (see docs/api.md). Binds 127.0.0.1 only unless --lan is given.\n"
    "A random bearer token is generated per run and printed to stderr (or pass --token / set\n"
    "HCS_API_TOKEN). Every route except GET /hcs/v1/health requires it. The embedded web UI\n"
    "(GET / and /ui/*, no token needed for the static page) is served on the same address.\n"
    "\n"
    "Options:\n"
    "  --host <addr>            Bind address (default 127.0.0.1; non-loopback requires --lan)\n"
    "  --port <n>               Port (default 8765; 0 = pick a free port)\n"
    "  --lan                    Explicit opt-in for non-loopback binding (0.0.0.0 unless --host is given)\n"
    "  --token <secret>         Use this bearer token (min 16 chars) instead of a random one\n"
    "  --allow-host <name>      Extra accepted Host header name (repeatable)\n"
    "  --allow-origin <origin>  Extra accepted Origin, e.g. http://127.0.0.1:3000 (repeatable)\n"
    "  --model-root <dir>      Approved model directory (repeatable; adds to settings.json roots)\n"
    "  --max-body-bytes <n>     Request body cap (default 1048576)\n"
    "  --max-inflight <n>       Concurrent request cap (default 8)\n"
    "  --timeout-s <n>          Per-request generation timeout (default 300)\n"
    "  --chat-template <t>      auto (default: model template for single-turn, ChatML otherwise) | chatml | "
    "plain\n"
    "  --help                   This text\n";

bool parse_uint(const std::string& s, unsigned long long max, unsigned long long& out) {
    if (s.empty() || s.size() > 18)
        return false;
    unsigned long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v > max)
        return false;
    out = v;
    return true;
}
}  // namespace

int run_serve(const std::vector<std::string>& args, const std::filesystem::path& state_dir, std::ostream& out,
              std::ostream& err) {
    auto bad = [&](const std::string& m) {
        err << "error: " << m << " [invalid_argument]\n";
        return exit_code(ErrorCode::InvalidArgument);
    };
    std::string host;
    unsigned long long port = 8765, body_cap = 1024ull * 1024, inflight = 8, timeout_s = 300;
    bool lan = false;
    std::string token, tmpl = "auto";
    std::vector<std::string> roots, hosts, origins;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](std::string& dst) {
            if (i + 1 >= args.size())
                return false;
            dst = args[++i];
            return true;
        };
        std::string v;
        if (a == "--help" || a == "-h") {
            out << kServeHelp;
            return 0;
        }
        if (a == "--lan")
            lan = true;
        else if (a == "--host") {
            if (!need(host))
                return bad("--host needs a value");
        } else if (a == "--token") {
            if (!need(token))
                return bad("--token needs a value");
        } else if (a == "--chat-template") {
            if (!need(tmpl))
                return bad("--chat-template needs a value");
        } else if (a == "--model-root") {
            if (!need(v))
                return bad("--model-root needs a value");
            roots.push_back(v);
        } else if (a == "--allow-host") {
            if (!need(v))
                return bad("--allow-host needs a value");
            hosts.push_back(v);
        } else if (a == "--allow-origin") {
            if (!need(v))
                return bad("--allow-origin needs a value");
            origins.push_back(v);
        } else if (a == "--port") {
            if (!need(v) || !parse_uint(v, 65535, port))
                return bad("--port must be 0..65535");
        } else if (a == "--max-body-bytes") {
            if (!need(v) || !parse_uint(v, 1ull << 30, body_cap) || body_cap == 0)
                return bad("--max-body-bytes must be 1..1073741824");
        } else if (a == "--max-inflight") {
            if (!need(v) || !parse_uint(v, 1024, inflight) || inflight == 0)
                return bad("--max-inflight must be 1..1024");
        } else if (a == "--timeout-s") {
            if (!need(v) || !parse_uint(v, 86400, timeout_s) || timeout_s == 0)
                return bad("--timeout-s must be 1..86400");
        } else
            return bad("unknown option: " + a);
    }
    if (tmpl != "auto" && tmpl != "chatml" && tmpl != "plain")
        return bad("--chat-template must be auto, chatml or plain");
    if (token.empty()) {
        if (const char* e = std::getenv("HCS_API_TOKEN"); e && *e)
            token = e;
    }
    const bool token_given = !token.empty();
    if (token_given && token.size() < 16)
        return bad("token must be at least 16 characters");
    if (!token_given)
        token = generate_token();

    if (host.empty())
        host = lan ? "0.0.0.0" : "127.0.0.1";
    Settings probe;
    probe.bind_host = host;
    probe.allow_lan = lan;
    probe.port = port == 0 ? 1 : static_cast<int>(port);
    if (auto v = validate(probe); !v) {
        err << "error: " << v.error().message << " [" << to_string(v.error().code) << "]\n";
        return v.error().exit_code();
    }
    if (lan && token.empty())
        return bad("--lan requires a token");

    Logger& log = global_logger();
    RuntimeOptions ro;
    ro.state_dir = state_dir;
    ro.model_roots = roots;
    ro.control.chat_template = tmpl;
    auto rt = Runtime::create(std::move(ro), log);
    if (!rt) {
        err << "error: " << rt.error().message << " [" << to_string(rt.error().code) << "]\n";
        return rt.error().exit_code();
    }

    ApiOptions ao;
    ao.token = token;
    ao.lan = lan;
    ao.bind_host = host;
    ao.allowed_hosts = hosts;
    ao.allowed_origins = origins;
    ao.max_body_bytes = static_cast<std::size_t>(body_cap);
    ao.max_inflight = static_cast<std::size_t>(inflight);
    ao.request_timeout = std::chrono::seconds(timeout_s);
    ApiHandler handler(*rt.value()->control, ao, log);

    TransportOptions to;
    to.host = host;
    to.port = static_cast<int>(port);
    to.max_body_bytes = ao.max_body_bytes;
    to.threads = std::max<std::size_t>(4, ao.max_inflight + 2);
    to.max_queued = 16;  // connections beyond threads + 16 are refused at accept time (M10 audit D2)
    HttpServer server(handler, to, log);
    auto bound = server.start();
    if (!bound) {
        err << "error: " << bound.error().message << " [" << to_string(bound.error().code) << "]\n";
        return bound.error().exit_code();
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    err << "HCSInfer API listening on http://" << host << ":" << bound.value() << "\n";
    if (!token_given)
        err << "HCSInfer API token: " << token << "\n";
    err << "HCSInfer web UI: http://" << (host == "0.0.0.0" ? std::string("127.0.0.1") : host) << ":"
        << bound.value()
        << "/  (enter the token on the page, or open it with #token=<token> appended; the fragment is never "
           "sent to the server)\n";
    if (lan)
        err << "warning: --lan exposes the API to your network; keep the token secret\n";
    if (!rt.value()->control->has_engine())
        err << "note: no inference engine is linked into this build; chat and model loading report "
               "'unsupported'\n";
    if (rt.value()->guard.empty())
        err << "note: no approved model roots; pass --model-root <dir> (or set model_roots in settings.json) "
               "to allow loading\n";
    err.flush();
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    err << "shutting down\n";
    server.stop();
    rt.value()->queue->shutdown();
    rt.value()->engine_queue->shutdown();
    return 0;
}

}  // namespace hcs::api
