// `HCSInfer benchmark --suite quick` - a real text-model benchmark through the llama engine adapter. It
// measures what the engine reports (load time, time to first token, prompt and decode tokens/s) and emits the
// AGENTS.md 21.1 schema. Nothing is estimated: values that cannot be measured are the string "unknown" /
// "not_applicable".
#include "benchmark_cmd.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <ostream>
#include <regex>
#include <sstream>

#include "hcs/fs_util.hpp"
#include "hcs/model_registry.hpp"
#include "hcs/result.hpp"
#include "hcs/version.hpp"

#ifdef HCS_HAVE_LLAMA
#include "hcs/engines/llama_engine.hpp"
#include "hcs/job_queue.hpp"
#endif
#ifdef HCS_HAVE_HARDWARE
#include "hcs/hardware/doctor.hpp"
#endif
#ifdef HCS_HAVE_STORAGE
#include "hcs/storage/sha256.hpp"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;

const char* benchmark_help() {
    return "Usage: HCSInfer benchmark --suite quick --model <path-or-id> [options]\n"
           "\n"
           "Benchmarks a real GGUF text model through the llama.cpp adapter and prints one JSON document\n"
           "(AGENTS.md 21.1, schema_version 1). Two phases are reported separately:\n"
           "  cold  the first load and first generation of this process (no warm-up). Engine/driver start-up "
           "costs\n"
           "        are included. The OS file cache is NOT flushed (that needs administrator rights), so "
           "'cold'\n"
           "        means process-cold; \"file_cache\" says so in the output.\n"
           "  warm  after --warmup discarded generations, --iterations measured generations (median/p90) "
           "and\n"
           "        --iterations unload/load cycles (load_ms median/p90, file cache warm).\n"
           "\n"
           "Options:\n"
           "  --suite quick          Only 'quick' exists (1 warm-up, 3 iterations, 64 new tokens); 'full' is "
           "planned\n"
           "  --model <path-or-id>   GGUF file path or model id from `models scan` (required)\n"
           "  --backend <b>          auto (default) | cpu | vulkan (no silent fallback when explicit)\n"
           "  --device <name>        GPU device substring\n"
           "  --ctx <n>              Context tokens (default 4096)\n"
           "  --max-tokens <n>       New tokens per generation (default 64, 1..4096)\n"
           "  --warmup <n>           Discarded warm-up generations (default 1, 0..20)\n"
           "  --iterations <n>       Measured iterations (default 3, 1..50)\n"
           "  --threads <n>          CPU threads (default: engine default)\n"
           "  --seed <n>             Sampling seed (default 42; temperature is 0, i.e. greedy)\n"
           "  --export <file>        Also write the JSON to this file (atomic, refuses to overwrite without "
           "--force)\n"
           "  --force                Allow --export to overwrite an existing file\n"
           "  --no-hash              Skip the model SHA-256 (it re-reads the whole file after the "
           "measurements)\n"
           "\n"
           "The prompt is a fixed text (never logged); the JSON records the real prompt token count.\n";
}

namespace {

int fail(std::ostream& err, const Error& e) {
    err << "error: " << e.message << " [" << to_string(e.code) << "]\n";
    return e.exit_code();
}
int fail(std::ostream& err, ErrorCode c, const std::string& m) {
    return fail(err, Error(c, m));
}

bool take_value(std::vector<std::string>& a, const std::string& flag, std::string& value, bool& bad) {
    auto it = std::find(a.begin(), a.end(), flag);
    if (it == a.end())
        return false;
    if (it + 1 == a.end()) {
        bad = true;
        return true;
    }
    value = *(it + 1);
    a.erase(it, it + 2);
    return true;
}
bool take_flag(std::vector<std::string>& a, const std::string& flag) {
    auto it = std::find(a.begin(), a.end(), flag);
    if (it == a.end())
        return false;
    a.erase(it);
    return true;
}

fs::path state_dir(const std::string& override_dir) {
    if (!override_dir.empty())
        return fs::path(override_dir);
    if (const char* e = std::getenv("HCS_STATE_DIR"); e && *e)
        return fs::path(e);
    if (const char* l = std::getenv("LOCALAPPDATA"); l && *l)
        return fs::path(l) / "HCSInfer";
    if (const char* h = std::getenv("HOME"); h && *h)
        return fs::path(h) / ".hcsinfer";
    return fs::path(".hcsinfer");
}

Result<std::string> resolve_model(const std::string& spec, const std::string& state_override) {
    std::error_code ec;
    if (spec.find("..") != std::string::npos && fs::exists(spec, ec))
        return Error(ErrorCode::SecurityViolation, "model path must not contain '..'");
    if (fs::is_regular_file(spec, ec))
        return fs::weakly_canonical(spec, ec).string();
    const fs::path reg_file = state_dir(state_override) / "registry.json";
    if (fs::exists(reg_file, ec)) {
        auto text = read_file_limited(reg_file, 64u << 20);
        if (!text)
            return text.error();
        ModelRegistry reg;
        if (auto r = reg.load_json(text.value()); !r)
            return r.error();
        if (auto m = reg.get(spec); m)
            return m.value().path;
    }
    return Error(ErrorCode::NotFound,
                 "model '" + spec + "' is neither an existing file nor a registered model id");
}

double percentile(std::vector<double> v, double p) {  // nearest-rank, p in (0,1]
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size())));
    return v[std::min(v.size(), std::max<std::size_t>(1, rank)) - 1];
}
double median(std::vector<double> v) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

json stats(const std::vector<double>& v) {
    return {{"median", median(v)},
            {"p90", percentile(v, 0.9)},
            {"min", v.empty() ? 0.0 : *std::min_element(v.begin(), v.end())},
            {"max", v.empty() ? 0.0 : *std::max_element(v.begin(), v.end())},
            {"samples", v}};
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string compiler_string() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#elif defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

struct ProcessMemory {
    std::uint64_t peak_working_set = 0;
    std::uint64_t peak_commit = 0;
    bool known = false;
};
ProcessMemory process_memory() {
    ProcessMemory m;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS c{};
    c.cb = sizeof c;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof c)) {
        m.peak_working_set = c.PeakWorkingSetSize;
        m.peak_commit = c.PeakPagefileUsage;
        m.known = true;
    }
#endif
    return m;
}

#ifdef HCS_HAVE_LLAMA
struct Sample {
    double ttft_ms = 0, prompt_tps = 0, decode_tps = 0;
    std::uint64_t prompt_tokens = 0, decode_tokens = 0;
    double wall_ms = 0;
};

std::string benchmark_prompt() {
    // ~130 tokens of neutral prose; fixed so every run sees identical input.
    std::string s;
    for (int i = 0; i < 12; ++i)
        s += "The harbour town woke slowly as the fishing boats returned and the market stalls opened. ";
    s += "Continue the story in two sentences.";
    return s;
}
#endif

}  // namespace

#ifndef HCS_HAVE_LLAMA

int run_benchmark(std::vector<std::string> args, const std::string&, std::ostream& out, std::ostream& err) {
    for (const auto& a : args)
        if (a == "--help" || a == "-h") {
            out << benchmark_help();
            return 0;
        }
    err << "error: this build has no llama engine (external/llama.cpp missing) [unsupported]\n";
    return exit_code(ErrorCode::Unsupported);
}

#else

int run_benchmark(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
                  std::ostream& err) {
    if (std::any_of(args.begin(), args.end(),
                    [](const std::string& a) { return a == "--help" || a == "-h"; })) {
        out << benchmark_help();
        return 0;
    }
    std::string suite, model, backend = "auto", device, v_ctx, v_max, v_warm, v_iter, v_thr, v_seed,
                              export_path;
    bool bad = false;
    take_value(args, "--suite", suite, bad);
    take_value(args, "--model", model, bad);
    take_value(args, "--backend", backend, bad);
    take_value(args, "--device", device, bad);
    take_value(args, "--ctx", v_ctx, bad);
    take_value(args, "--max-tokens", v_max, bad);
    take_value(args, "--warmup", v_warm, bad);
    take_value(args, "--iterations", v_iter, bad);
    take_value(args, "--threads", v_thr, bad);
    take_value(args, "--seed", v_seed, bad);
    take_value(args, "--export", export_path, bad);
    const bool force = take_flag(args, "--force");
    const bool no_hash = take_flag(args, "--no-hash");
    if (bad)
        return fail(err, ErrorCode::InvalidArgument, "an option is missing its value");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    if (suite.empty())
        return fail(err, ErrorCode::InvalidArgument, "--suite is required (only 'quick' is implemented)");
    if (suite == "full")
        return fail(err, ErrorCode::Unsupported,
                    "benchmark --suite full is planned and not implemented yet; use --suite quick");
    if (suite != "quick")
        return fail(err, ErrorCode::InvalidArgument, "--suite must be quick");
    if (model.empty())
        return fail(err, ErrorCode::InvalidArgument, "--model is required");

    LoadRequest lr;
    std::uint32_t max_tokens = 64;
    std::uint32_t warmup = 1, iterations = 3;
    std::uint64_t seed = 42;
    auto parse_u = [&](const std::string& t, std::uint64_t lo, std::uint64_t hi, std::uint64_t& out_v,
                       const char* name) -> bool {
        if (t.empty() || t.size() > 12 || t.find_first_not_of("0123456789") != std::string::npos)
            return false;
        const std::uint64_t v = std::stoull(t);
        if (v < lo || v > hi)
            return false;
        out_v = v;
        (void)name;
        return true;
    };
    {
        std::uint64_t v = 0;
        if (!v_ctx.empty()) {
            if (!parse_u(v_ctx, 128, 1048576, v, "ctx"))
                return fail(err, ErrorCode::InvalidArgument, "--ctx must be 128..1048576");
            lr.context_tokens = static_cast<std::uint32_t>(v);
        }
        if (!v_max.empty()) {
            if (!parse_u(v_max, 1, 4096, v, "max"))
                return fail(err, ErrorCode::InvalidArgument, "--max-tokens must be 1..4096");
            max_tokens = static_cast<std::uint32_t>(v);
        }
        if (!v_warm.empty()) {
            if (!parse_u(v_warm, 0, 20, v, "warmup"))
                return fail(err, ErrorCode::InvalidArgument, "--warmup must be 0..20");
            warmup = static_cast<std::uint32_t>(v);
        }
        if (!v_iter.empty()) {
            if (!parse_u(v_iter, 1, 50, v, "iter"))
                return fail(err, ErrorCode::InvalidArgument, "--iterations must be 1..50");
            iterations = static_cast<std::uint32_t>(v);
        }
        if (!v_thr.empty()) {
            if (!parse_u(v_thr, 1, 1024, v, "thr"))
                return fail(err, ErrorCode::InvalidArgument, "--threads must be 1..1024");
            lr.threads = static_cast<std::int32_t>(v);
        }
        if (!v_seed.empty()) {
            if (!parse_u(v_seed, 0, UINT64_MAX / 2, v, "seed"))
                return fail(err, ErrorCode::InvalidArgument, "--seed must be a non-negative integer");
            seed = v;
        }
    }
    if (backend == "cpu")
        lr.backend = Backend::Cpu;
    else if (backend == "vulkan")
        lr.backend = Backend::Vulkan;
    else if (backend == "auto")
        lr.backend = Backend::Auto;
    else
        return fail(err, ErrorCode::InvalidArgument, "--backend must be auto, cpu or vulkan");
    lr.device = device;
    if (!export_path.empty()) {
        std::error_code ec;
        if (fs::exists(export_path, ec) && !force)
            return fail(err, ErrorCode::AlreadyExists, "export file exists (use --force to overwrite)");
    }

    auto path = resolve_model(model, state_override);
    if (!path)
        return fail(err, path.error());
    lr.model_path = path.value();
    lr.model_id = fs::path(path.value()).stem().string();
    std::error_code ec;
    const std::uint64_t model_size = fs::file_size(lr.model_path, ec);

    using clock = std::chrono::steady_clock;
    JobQueue queue;
    engines::LlamaTextEngine engine(queue);
    const std::string prompt = benchmark_prompt();

    auto timed_load = [&](double& ms) -> Result<void> {
        const auto t0 = clock::now();
        auto r = engine.load(lr);
        ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        return r;
    };
    auto one_generation = [&](Sample& s) -> Result<void> {
        TextRequest tr;
        tr.prompt = prompt;
        tr.max_tokens = max_tokens;
        tr.temperature = 0.0f;
        tr.seed = seed;
        tr.apply_chat_template = false;
        const auto t0 = clock::now();
        auto id = engine.generate(tr, [](const JobEvent&) {});
        if (!id)
            return id.error();
        while (!queue.wait(id.value(), std::chrono::milliseconds(50))) {
        }
        s.wall_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        auto st = queue.status(id.value());
        if (!st)
            return st.error();
        if (st.value().state != JobState::Succeeded)
            return st.value().error ? *st.value().error
                                    : Error(ErrorCode::BackendError, "generation did not succeed");
        const EngineMetrics m = engine.metrics();
        s.ttft_ms = m.ttft_ms;
        s.prompt_tps = m.prompt_tokens_per_second;
        s.decode_tps = m.decode_tokens_per_second;
        s.prompt_tokens = m.prompt_tokens;
        s.decode_tokens = m.decode_tokens;
        if (s.decode_tokens == 0)
            return Error(ErrorCode::BackendError, "the generation produced no tokens");
        return {};
    };

    // ---- cold: first load + first generation of this process
    // -------------------------------------------------
    double cold_load_ms = 0;
    if (auto r = timed_load(cold_load_ms); !r)
        return fail(err, r.error());
    const EngineMetrics after_load = engine.metrics();
    Sample cold;
    if (auto r = one_generation(cold); !r) {
        (void)engine.unload();
        return fail(err, r.error());
    }
    const ProcessMemory mem_after_cold = process_memory();
    err << "[hcsinfer] cold: backend=" << after_load.backend << " device=" << after_load.device
        << " load_ms=" << static_cast<long>(cold_load_ms) << " ttft_ms=" << static_cast<long>(cold.ttft_ms)
        << "\n";

    // ---- warm: discarded warm-up, then measured iterations
    // ----------------------------------------------------
    for (std::uint32_t i = 0; i < warmup; ++i) {
        Sample w;
        if (auto r = one_generation(w); !r) {
            (void)engine.unload();
            return fail(err, r.error());
        }
    }
    std::vector<double> ttft, ptps, dtps, wall;
    Sample last;
    for (std::uint32_t i = 0; i < iterations; ++i) {
        Sample s;
        if (auto r = one_generation(s); !r) {
            (void)engine.unload();
            return fail(err, r.error());
        }
        ttft.push_back(s.ttft_ms);
        ptps.push_back(s.prompt_tps);
        dtps.push_back(s.decode_tps);
        wall.push_back(s.wall_ms);
        last = s;
        err << "[hcsinfer] warm " << (i + 1) << "/" << iterations
            << ": ttft_ms=" << static_cast<long>(s.ttft_ms) << " prompt_tps=" << s.prompt_tps
            << " decode_tps=" << s.decode_tps << "\n";
    }
    std::vector<double> warm_loads;
    for (std::uint32_t i = 0; i < iterations; ++i) {
        if (auto r = engine.unload(); !r)
            return fail(err, r.error());
        double ms = 0;
        if (auto r = timed_load(ms); !r)
            return fail(err, r.error());
        warm_loads.push_back(ms);
    }
    (void)engine.unload();
    const ProcessMemory mem_end = process_memory();

    // ---- machine description (after the measurements so it cannot disturb them)
    // ------------------------------
    json summary = json::object(), storage = json::object();
#ifdef HCS_HAVE_HARDWARE
    {
        hcs::hardware::DoctorOptions d;
        d.json = true;
        d.no_vulkan = lr.backend == Backend::Cpu;
        d.storage_root = lr.model_path;
        bool ok = false;
        const std::string text = hcs::hardware::collect_report_json(d, &ok);
        auto j = json::parse(text, nullptr, false);
        if (ok && !j.is_discarded()) {
            if (j.contains("summary"))
                summary = j["summary"];
            if (j.contains("storage") && j["storage"].contains("requested_volume"))
                storage = j["storage"]["requested_volume"];
        }
    }
#endif
    auto sv = [&](const char* k) -> json { return summary.contains(k) ? summary[k] : json("unknown"); };

    std::string sha = "unknown (--no-hash)";
#ifdef HCS_HAVE_STORAGE
    if (!no_hash) {
        std::ifstream f(lr.model_path, std::ios::binary);
        if (!f) {
            sha = "unknown (model not re-readable)";
        } else {
            hcs::storage::Sha256 h;
            std::vector<char> buf(4u << 20);
            while (f.read(buf.data(), static_cast<std::streamsize>(buf.size())) || f.gcount() > 0)
                h.update(buf.data(), static_cast<std::size_t>(f.gcount()));
            sha = hcs::storage::to_hex(h.finalize());
        }
    }
#else
    (void)no_hash;
    sha = "unknown (storage module not built)";
#endif

    std::string backend_name = "unknown";
    if (after_load.backend == "cpu")
        backend_name = "CPU";
    else if (after_load.backend == "vulkan")
        backend_name = (after_load.offloaded_layers >= 0 && after_load.total_layers > 0 &&
                        after_load.offloaded_layers < after_load.total_layers)
                           ? "Hybrid"
                           : "Vulkan";

    std::string storage_source = "unknown";
    if (!storage.empty()) {
        storage_source = storage.value("bus_type", std::string("unknown")) + " (" +
                         storage.value("filesystem", std::string("unknown")) + ", rotational: " +
                         (storage.contains("rotational") && storage["rotational"].is_string()
                              ? storage["rotational"].get<std::string>()
                              : std::string("unknown")) +
                         ")";
    }

    auto base = [&]() {
        json j;
        j["schema_version"] = 1;
        j["timestamp_utc"] = utc_now();
#ifdef HCS_GIT_COMMIT
        j["git_commit"] = HCS_GIT_COMMIT;
#else
        j["git_commit"] = "unknown";
#endif
#ifdef HCS_BUILD_TYPE
        j["build_type"] = HCS_BUILD_TYPE;
#else
        j["build_type"] = "unknown";
#endif
        j["compiler"] = compiler_string();
#ifdef HCS_CMAKE_VERSION
        j["cmake_version"] = HCS_CMAKE_VERSION;
#else
        j["cmake_version"] = "unknown";
#endif
        j["os_build"] = sv("os_build");
        j["cpu"] = sv("cpu");
        j["gpu"] = sv("gpu");
        j["gpu_vendor_id"] = sv("gpu_vendor_id");
        j["gpu_device_id"] = sv("gpu_device_id");
        j["driver_version"] = sv("driver_version");
        j["vulkan_api_version"] = sv("vulkan_api_version");
        j["ram_bytes"] = sv("ram_bytes");
        j["model_sha256"] = sha;
        j["model_file"] = fs::path(lr.model_path).filename().string();
        j["model_size_bytes"] = ec ? json("unknown") : json(model_size);
        j["model_format"] = "GGUF";
        {
            // The GGUF header probe does not expose general.file_type, so this is read from the file name.
            static const std::regex q(R"((iq\d\w*|q\d_[0-9a-z_]+|q\d|bf16|f16|f32))", std::regex::icase);
            std::smatch m;
            const std::string stem = fs::path(lr.model_path).stem().string();
            j["quantization"] = std::regex_search(stem, m, q) ? json(m[1].str()) : json("unknown");
            j["quantization_source"] = "file name";
        }
        j["context_tokens"] = lr.context_tokens;
        j["backend"] = backend_name;
        j["device"] = after_load.device;
        j["offloaded_layers"] = after_load.offloaded_layers;
        j["total_layers"] = after_load.total_layers;
        j["storage_source"] = storage_source;
        j["hdd_read_mib_s_median"] = "unknown";
        j["decompression_mib_s_median"] = "not_applicable";
        j["file_cache"] = "not_controlled";
        j["max_new_tokens"] = max_tokens;
        j["seed"] = seed;
        j["temperature"] = 0.0;
        return j;
    };

    json cold_rec = base();
    cold_rec["cold_or_warm"] = "cold";
    cold_rec["warmup_iterations"] = 0;
    cold_rec["measured_iterations"] = 1;
    cold_rec["load_ms_median"] = cold_load_ms;
    cold_rec["load_ms_p90"] = cold_load_ms;
    cold_rec["ttft_ms_median"] = cold.ttft_ms;
    cold_rec["ttft_ms_p90"] = cold.ttft_ms;
    cold_rec["prompt_tokens"] = cold.prompt_tokens;
    cold_rec["decode_tokens"] = cold.decode_tokens;
    cold_rec["prompt_tokens_per_second_median"] = cold.prompt_tps;
    cold_rec["decode_tokens_per_second_median"] = cold.decode_tps;
    if (mem_after_cold.known) {
        cold_rec["peak_ram_bytes"] = mem_after_cold.peak_working_set;
        cold_rec["peak_commit_bytes"] = mem_after_cold.peak_commit;
    } else {
        cold_rec["peak_ram_bytes"] = "unknown";
    }
    cold_rec["result_status"] = "PASS";
    cold_rec["note"] = "process-cold: first load and first generation, OS file cache not flushed";

    json rec = base();
    rec["cold_or_warm"] = "warm";
    rec["warmup_iterations"] = warmup;
    rec["measured_iterations"] = iterations;
    rec["load_ms_median"] = median(warm_loads);
    rec["load_ms_p90"] = percentile(warm_loads, 0.9);
    rec["ttft_ms_median"] = median(ttft);
    rec["ttft_ms_p90"] = percentile(ttft, 0.9);
    rec["prompt_tokens"] = last.prompt_tokens;
    rec["decode_tokens"] = last.decode_tokens;
    rec["prompt_tokens_per_second_median"] = median(ptps);
    rec["prompt_tokens_per_second_p90"] = percentile(ptps, 0.9);
    rec["decode_tokens_per_second_median"] = median(dtps);
    rec["decode_tokens_per_second_p90"] = percentile(dtps, 0.9);
    rec["wall_ms_median"] = median(wall);
    if (mem_end.known) {
        rec["peak_ram_bytes"] = mem_end.peak_working_set;
        rec["peak_commit_bytes"] = mem_end.peak_commit;
    } else {
        rec["peak_ram_bytes"] = "unknown";
    }
    rec["peak_ram_note"] =
        "process peak working set since start (includes the cold phase and the warm reloads)";
    rec["samples"] = {{"ttft_ms", stats(ttft)},
                      {"prompt_tokens_per_second", stats(ptps)},
                      {"decode_tokens_per_second", stats(dtps)},
                      {"load_ms", stats(warm_loads)},
                      {"wall_ms", stats(wall)}};
    rec["result_status"] = "PASS";
    rec["suite"] = suite;
    rec["benchmark_id"] = lr.backend == Backend::Cpu ? "B0-text" : "B1-text";
    rec["cold"] = std::move(cold_rec);
    rec["hcsinfer_version"] = HCS_VERSION_STRING;

    const std::string text = rec.dump(2) + "\n";
    if (!export_path.empty()) {
        if (auto w = atomic_write_file(export_path, text); !w)
            return fail(err, w.error());
        err << "[hcsinfer] wrote " << fs::path(export_path).filename().string() << "\n";
    }
    out << text;
    return 0;
}

#endif  // HCS_HAVE_LLAMA

}  // namespace hcs::cli
