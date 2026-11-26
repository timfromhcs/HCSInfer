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
#ifdef HCS_HAVE_STORAGE
#include "hcs/storage/vault_prefetch.hpp"
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

Runtime::~Runtime() {
    if (admission)
        admission->stop_watchdog();  // no kill switch while we are tearing down anyway
    // Jobs of both queues run closures that reference control/engines/registry: stop and join the workers
    // first.
    try {
        if (queue)
            queue->shutdown();
        if (engine_queue)
            engine_queue->shutdown();
    } catch (...) {
        (void)0;  // a destructor must not throw; shutdown() already joins what it can
    }
    control.reset();  // drops its job handles/callbacks before the engines it points to
}

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
    {
        const scheduler::AdmissionConfig ac = options.admission.value_or(scheduler::AdmissionConfig{});
        if (auto v = scheduler::validate(ac); !v)
            return v.error();
        rt->admission = std::make_shared<scheduler::AdmissionController>(
            ac, options.admission_probe ? options.admission_probe : telemetry::make_system_probe());
        co.admission = rt->admission;
    }
    if (options.vault_prefetch && !co.model_prefetch) {
#ifdef HCS_HAVE_STORAGE
        if (options.vault_buffers < 1 || options.vault_buffers > 16 || options.vault_chunk_mib < 1 ||
            options.vault_chunk_mib > 128)
            return make_error(ErrorCode::InvalidArgument, "vault prefetch: buffers 1..16, chunk 1..128 MiB");
        const std::size_t buffers = options.vault_buffers;
        const std::uint32_t chunk = options.vault_chunk_mib << 20;
        co.model_prefetch = [buffers, chunk](const std::filesystem::path& file, const std::string& expected,
                                             const CancellationToken& token) -> Result<nlohmann::json> {
            storage::PrefetchOptions po;
            po.buffers = buffers;
            po.chunk_bytes = chunk;
            po.expected_sha256 = expected;
            po.cancel = token;
            auto r = storage::prefetch_and_verify(file, po);
            if (!r)
                return r.error();
            const auto& v = r.value();
            return nlohmann::json{{"bytes", v.bytes},
                                  {"sha256", v.sha256},
                                  {"verified_against_manifest", v.verified},
                                  {"wall_seconds", v.wall_seconds},
                                  {"mib_per_second", v.mib_per_second},
                                  {"buffer_bytes", v.buffer_bytes}};
        };
#else
        return make_error(ErrorCode::Unsupported,
                          "vault prefetch needs the storage module, which is not built");
#endif
    }
    rt->control = std::make_unique<ControlPlane>(rt->registry, rt->guard, rt->budget, *rt->queue,
                                                 rt->engine.get(), logger, co, rt->image_engine.get());
    rt->admission->start_watchdog();
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
    "  --min-free-ram-mb <n>    Overload protection: refuse work that would leave less RAM free (default "
    "2560)\n"
    "  --hard-floor-mb <n>      Watchdog kill switch trips below this free RAM (default 1024)\n"
    "  --max-commit-pct <n>     Refuse work above this commit charge in percent (default 90)\n"
    "  --max-active-jobs <n>    Jobs in flight (running + queued) before 503 Retry-After (default 4)\n"
    "  --watchdog-ms <n>        Watchdog sampling interval (default 1000)\n"
    "  --no-overload-protection Disable admission control and the watchdog (not recommended)\n"
    "  --vault-prefetch         Opt-in Model Vault load stage: before the engine opens a model file it is "
    "read\n"
    "                           once, sequentially, through the bounded async reader and its SHA-256 is\n"
    "                           checked against the manifest digest when one exists. Nothing is written to "
    "disk.\n"
    "                           Default off: it is only worth it where measured (docs/model-vault.md)\n"
    "  --vault-buffers <n>      Read-ahead slots for --vault-prefetch, 1..16 (default 4)\n"
    "  --vault-chunk-mib <n>    Read size per slot for --vault-prefetch, 1..128 (default 16)\n"
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

TransportOptions serve_transport_options(const ApiOptions& api, const std::string& host, int port) {
    TransportOptions to;
    to.host = host;
    to.port = port;
    to.max_body_bytes = api.max_body_bytes;
    // Worker threads only block on socket reads until the handler's inflight limit applies, so a generous
    // pool is cheap. Connections beyond threads + max_queued are closed at accept time (M10 audit D2).
    to.threads = std::max<std::size_t>(48, api.max_inflight + 40);
    to.max_queued = 32;
    to.idle_timeout_s = 2;  // first byte of a request; connections that never send one are dropped
    to.read_timeout_s = 5;
    return to;
}

void graceful_shutdown(HttpServer& server, Runtime& runtime) {
    if (runtime.admission)
        runtime.admission->stop_watchdog();
    runtime.queue->shutdown();  // cancels running/queued jobs; blocked request threads get Cancelled
    runtime.engine_queue->shutdown();
    server.stop();  // now every connection thread can finish and be joined
}

int run_serve(const std::vector<std::string>& args, const std::filesystem::path& state_dir, std::ostream& out,
              std::ostream& err) {
    auto bad = [&](const std::string& m) {
        err << "error: " << m << " [invalid_argument]\n";
        return exit_code(ErrorCode::InvalidArgument);
    };
    std::string host;
    unsigned long long port = 8765, body_cap = 1024ull * 1024, inflight = 8, timeout_s = 300;
    bool lan = false;
    scheduler::AdmissionConfig adm;
    unsigned long long min_free_mb = adm.min_free_ram_bytes >> 20, hard_floor_mb = adm.hard_floor_bytes >> 20,
                       commit_pct = 90, max_jobs = adm.max_in_flight, watchdog_ms = 1000;
    bool overload_off = false;
    bool lan = false, vault = false;
    unsigned long long vault_buffers = 4, vault_chunk = 16;
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
        else if (a == "--vault-prefetch")
            vault = true;
        else if (a == "--vault-buffers") {
            if (!need(v) || !parse_uint(v, 16, vault_buffers) || vault_buffers == 0)
                return bad("--vault-buffers must be 1..16");
        } else if (a == "--vault-chunk-mib") {
            if (!need(v) || !parse_uint(v, 128, vault_chunk) || vault_chunk == 0)
                return bad("--vault-chunk-mib must be 1..128");
        } else if (a == "--host") {
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
        } else if (a == "--min-free-ram-mb") {
            if (!need(v) || !parse_uint(v, 1ull << 22, min_free_mb))
                return bad("--min-free-ram-mb must be 0..4194304");
        } else if (a == "--hard-floor-mb") {
            if (!need(v) || !parse_uint(v, 1ull << 22, hard_floor_mb))
                return bad("--hard-floor-mb must be 0..4194304");
        } else if (a == "--max-commit-pct") {
            if (!need(v) || !parse_uint(v, 100, commit_pct) || commit_pct == 0)
                return bad("--max-commit-pct must be 1..100");
        } else if (a == "--max-active-jobs") {
            if (!need(v) || !parse_uint(v, 1024, max_jobs) || max_jobs == 0)
                return bad("--max-active-jobs must be 1..1024");
        } else if (a == "--watchdog-ms") {
            if (!need(v) || !parse_uint(v, 600000, watchdog_ms) || watchdog_ms == 0)
                return bad("--watchdog-ms must be 1..600000");
        } else if (a == "--no-overload-protection") {
            overload_off = true;
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
    adm.enabled = !overload_off;
    adm.min_free_ram_bytes = min_free_mb << 20;
    adm.hard_floor_bytes = hard_floor_mb << 20;
    adm.max_commit_fraction = static_cast<double>(commit_pct) / 100.0;
    adm.hard_commit_fraction = std::max(adm.hard_commit_fraction, adm.max_commit_fraction);
    adm.max_in_flight = static_cast<std::size_t>(max_jobs);
    adm.watchdog_interval = std::chrono::milliseconds(watchdog_ms);
    if (auto v = scheduler::validate(adm); !v)
        return bad(v.error().message);
    ro.admission = adm;
    ro.vault_prefetch = vault;
    ro.vault_buffers = static_cast<std::size_t>(vault_buffers);
    ro.vault_chunk_mib = static_cast<std::uint32_t>(vault_chunk);
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

    const TransportOptions to = serve_transport_options(ao, host, static_cast<int>(port));
    HttpServer server(handler, to, log);
    auto bound = server.start();
    if (!bound) {
        err << "error: " << bound.error().message << " [" << to_string(bound.error().code) << "]\n";
        return bound.error().exit_code();
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_signal);  // Windows: Ctrl+Break and console close
#endif
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
    graceful_shutdown(server, *rt.value());
    return 0;
}

}  // namespace hcs::api
