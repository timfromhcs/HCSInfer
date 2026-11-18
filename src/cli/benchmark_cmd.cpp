// `HCSInfer benchmark --suite quick|full` - a real text-model benchmark through the llama engine adapter. The
// measurement itself lives in the shared runner (hcs/benchmark.hpp, also used by POST /hcs/v1/benchmarks);
// this file parses the command line, resolves the model and gathers the machine description. Nothing is
// estimated: values that cannot be measured are the string "unknown" / "not_applicable".
#include "benchmark_cmd.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <ostream>
#include <regex>
#include <sstream>

#include "hcs/benchmark.hpp"
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

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;

const char* benchmark_help() {
    return "Usage: HCSInfer benchmark --suite quick|full --model <path-or-id> [options]\n"
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
           "  --suite quick|full     quick: the cold/warm record above (1 warm-up, 3 iterations, 64 new "
           "tokens).\n"
           "                         full: quick PLUS a prefill sweep (TTFT and prompt tokens/s at several "
           "prompt\n"
           "                         sizes, 1 new token) and a decode sweep (decode tokens/s at the same "
           "sizes);\n"
           "                         every point has its own median/p90 and raw samples. Points that do not "
           "fit\n"
           "                         --ctx are recorded as SKIPPED with the reason.\n"
           "  --sweep-sizes <a,b,..> Full suite only: approximate prompt sizes in tokens, 1..8 values of\n"
           "                         16..1048576 (default 128,512,1024,2048). The real token count of every\n"
           "                         point is measured and recorded.\n"
           "  --model <path-or-id>   GGUF file path or model id from `models scan` (required)\n"
           "  --backend <b>          auto (default) | cpu | vulkan (no silent fallback when explicit)\n"
           "  --load-mode <m>        auto (default) | none | mmap | mlock | mmap+mlock | direct_io\n"
           "                         Weight-loading mode of the engine (AGENTS.md 11.2 Stage A). 'none' "
           "reads the\n"
           "                         weights into RAM, 'mmap' maps the file, 'mlock'/'mmap+mlock' lock pages "
           "in RAM,\n"
           "                         'direct_io' bypasses the OS file cache. Recorded in the JSON as "
           "\"load_mode\".\n"
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

namespace {

// IBenchTarget over the llama adapter: generation is a job in the adapter's own queue, metrics come from the
// adapter after the job finished.
class LlamaBenchTarget final : public bench::IBenchTarget {
public:
    LlamaBenchTarget() : engine_(queue_) {}

    Result<void> load(const LoadRequest& r, const CancellationToken&) override { return engine_.load(r); }
    Result<void> unload() override { return engine_.unload(); }
    EngineMetrics metrics() const override { return engine_.metrics(); }

    Result<bench::Sample> generate(const TextRequest& tr, const CancellationToken& token) override {
        auto id = engine_.generate(tr, [](const JobEvent&) {});
        if (!id)
            return id.error();
        bool cancel_sent = false;
        while (!queue_.wait(id.value(), std::chrono::milliseconds(50))) {
            if (token.cancelled() && !cancel_sent) {
                (void)queue_.cancel(id.value());
                cancel_sent = true;
            }
        }
        auto st = queue_.status(id.value());
        if (!st)
            return st.error();
        if (st.value().state == JobState::Cancelled)
            return Error(ErrorCode::Cancelled, "generation cancelled");
        if (st.value().state != JobState::Succeeded)
            return st.value().error ? *st.value().error
                                    : Error(ErrorCode::BackendError, "generation did not succeed");
        const EngineMetrics m = engine_.metrics();
        bench::Sample s;
        s.ttft_ms = m.ttft_ms;
        s.prompt_tps = m.prompt_tokens_per_second;
        s.decode_tps = m.decode_tokens_per_second;
        s.prompt_tokens = m.prompt_tokens;
        s.decode_tokens = m.decode_tokens;
        return s;
    }

private:
    JobQueue queue_;
    engines::LlamaTextEngine engine_;
};

bool parse_sizes(const std::string& text, std::vector<std::uint32_t>& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto comma = text.find(',', pos);
        const std::string tok =
            text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (tok.empty() || tok.size() > 8 || tok.find_first_not_of("0123456789") != std::string::npos)
            return false;
        out.push_back(static_cast<std::uint32_t>(std::stoul(tok)));
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return true;
}

}  // namespace

int run_benchmark(std::vector<std::string> args, const std::string& state_override, std::ostream& out,
                  std::ostream& err) {
    if (std::any_of(args.begin(), args.end(),
                    [](const std::string& a) { return a == "--help" || a == "-h"; })) {
        out << benchmark_help();
        return 0;
    }
    std::string suite, model, backend = "auto", device, load_mode, v_ctx, v_max, v_warm, v_iter, v_thr,
                              v_seed, export_path, v_sweep;
    bool bad = false;
    take_value(args, "--suite", suite, bad);
    take_value(args, "--model", model, bad);
    take_value(args, "--backend", backend, bad);
    take_value(args, "--load-mode", load_mode, bad);
    take_value(args, "--device", device, bad);
    take_value(args, "--ctx", v_ctx, bad);
    take_value(args, "--max-tokens", v_max, bad);
    take_value(args, "--warmup", v_warm, bad);
    take_value(args, "--iterations", v_iter, bad);
    take_value(args, "--threads", v_thr, bad);
    take_value(args, "--seed", v_seed, bad);
    take_value(args, "--export", export_path, bad);
    take_value(args, "--sweep-sizes", v_sweep, bad);
    const bool force = take_flag(args, "--force");
    const bool no_hash = take_flag(args, "--no-hash");
    if (bad)
        return fail(err, ErrorCode::InvalidArgument, "an option is missing its value");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    if (suite.empty())
        return fail(err, ErrorCode::InvalidArgument, "--suite is required (quick or full)");
    if (suite != "quick" && suite != "full")
        return fail(err, ErrorCode::InvalidArgument, "--suite must be quick or full");
    if (model.empty())
        return fail(err, ErrorCode::InvalidArgument, "--model is required");
    if (!v_sweep.empty() && suite != "full")
        return fail(err, ErrorCode::InvalidArgument, "--sweep-sizes needs --suite full");

    LoadRequest lr;
    bench::Plan plan;
    plan.suite = suite == "full" ? bench::Suite::Full : bench::Suite::Quick;
    auto parse_u = [&](const std::string& t, std::uint64_t lo, std::uint64_t hi,
                       std::uint64_t& out_v) -> bool {
        if (t.empty() || t.size() > 12 || t.find_first_not_of("0123456789") != std::string::npos)
            return false;
        const std::uint64_t v = std::stoull(t);
        if (v < lo || v > hi)
            return false;
        out_v = v;
        return true;
    };
    {
        std::uint64_t v = 0;
        if (!v_ctx.empty()) {
            if (!parse_u(v_ctx, 128, 1048576, v))
                return fail(err, ErrorCode::InvalidArgument, "--ctx must be 128..1048576");
            lr.context_tokens = static_cast<std::uint32_t>(v);
        }
        if (!v_max.empty()) {
            if (!parse_u(v_max, 1, 4096, v))
                return fail(err, ErrorCode::InvalidArgument, "--max-tokens must be 1..4096");
            plan.max_tokens = static_cast<std::uint32_t>(v);
        }
        if (!v_warm.empty()) {
            if (!parse_u(v_warm, 0, 20, v))
                return fail(err, ErrorCode::InvalidArgument, "--warmup must be 0..20");
            plan.warmup = static_cast<std::uint32_t>(v);
        }
        if (!v_iter.empty()) {
            if (!parse_u(v_iter, 1, 50, v))
                return fail(err, ErrorCode::InvalidArgument, "--iterations must be 1..50");
            plan.iterations = static_cast<std::uint32_t>(v);
        }
        if (!v_thr.empty()) {
            if (!parse_u(v_thr, 1, 1024, v))
                return fail(err, ErrorCode::InvalidArgument, "--threads must be 1..1024");
            lr.threads = static_cast<std::int32_t>(v);
        }
        if (!v_seed.empty()) {
            if (!parse_u(v_seed, 0, UINT64_MAX / 2, v))
                return fail(err, ErrorCode::InvalidArgument, "--seed must be a non-negative integer");
            plan.seed = v;
        }
    }
    if (!v_sweep.empty() && !parse_sizes(v_sweep, plan.sweep_sizes))
        return fail(err, ErrorCode::InvalidArgument,
                    "--sweep-sizes must be a comma-separated list of integers");
    if (auto v = bench::validate_plan(plan); !v)
        return fail(err, v.error());
    if (backend == "cpu")
        lr.backend = Backend::Cpu;
    else if (backend == "vulkan")
        lr.backend = Backend::Vulkan;
    else if (backend == "auto")
        lr.backend = Backend::Auto;
    else
        return fail(err, ErrorCode::InvalidArgument, "--backend must be auto, cpu or vulkan");
    lr.device = device;
    lr.load_mode = load_mode;  // "" => engine default; the engine validates the value
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

    LlamaBenchTarget target;
    const bench::ProgressFn progress = [&](double, const std::string& message) {
        err << "[hcsinfer] " << message << "\n";
    };

    // Machine description, gathered by the runner AFTER the measurements.
    const bench::MetaFn meta = [&]() {
        bench::Meta m;
        json storage = json::object();
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
                    m.summary = j["summary"];
                if (j.contains("storage") && j["storage"].contains("requested_volume"))
                    storage = j["storage"]["requested_volume"];
            }
        }
#endif
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
        m.model_sha256 = sha;
        m.model_file = fs::path(lr.model_path).filename().string();
        m.model_size_bytes = ec ? json("unknown") : json(model_size);
        {
            // The GGUF header probe does not expose general.file_type, so this is read from the file name.
            static const std::regex q(R"((iq\d\w*|q\d_[0-9a-z_]+|q\d|bf16|f16|f32))", std::regex::icase);
            std::smatch sm;
            const std::string stem = fs::path(lr.model_path).stem().string();
            m.quantization = std::regex_search(stem, sm, q) ? sm[1].str() : "unknown";
            m.quantization_source = "file name";
        }
        if (!storage.empty()) {
            m.storage_source = storage.value("bus_type", std::string("unknown")) + " (" +
                               storage.value("filesystem", std::string("unknown")) + ", rotational: " +
                               (storage.contains("rotational") && storage["rotational"].is_string()
                                    ? storage["rotational"].get<std::string>()
                                    : std::string("unknown")) +
                               ")";
        }
        return m;
    };

    auto result = bench::run_suite(target, lr, plan, meta, CancellationToken{}, progress);
    if (!result)
        return fail(err, result.error());

    const std::string text = result.value().dump(2) + "\n";
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
