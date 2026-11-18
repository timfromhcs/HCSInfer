#include "hcs/benchmark.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

#include "hcs/version.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace hcs::bench {

using nlohmann::json;

namespace {

constexpr std::uint32_t kMaxSweepPoints = 8;
constexpr std::uint32_t kMinSweepTokens = 16;
constexpr std::uint32_t kMaxSweepTokens = 1048576;
// Headroom kept free in the context for the generated tokens' template/BOS overhead.
constexpr std::uint32_t kContextMargin = 8;
// Used until the first measured prompt gives a real figure (the real token count is always recorded).
constexpr double kDefaultTokensPerSentence = 16.0;

const char* const kSentence =
    "The harbour town woke slowly as the fishing boats returned and the market stalls opened. ";

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

using clock_type = std::chrono::steady_clock;

struct Runner {
    IBenchTarget& target;
    const LoadRequest& load;
    const Plan& plan;
    const CancellationToken& token;
    const ProgressFn& progress;
    double steps_total = 1.0;
    double steps_done = 0.0;

    void step(const std::string& message) {
        steps_done += 1.0;
        if (progress)
            progress(std::min(1.0, steps_done / steps_total), message);
    }

    Result<void> check_cancel() const {
        if (token.cancelled())
            return make_error(ErrorCode::Cancelled, "benchmark cancelled");
        return {};
    }

    Result<double> timed_load() {
        if (auto c = check_cancel(); !c)
            return c.error();
        const auto t0 = clock_type::now();
        auto r = target.load(load, token);
        const double ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
        if (!r)
            return r.error();
        return ms;
    }

    Result<Sample> one_generation(const std::string& prompt, std::uint32_t max_tokens) {
        if (auto c = check_cancel(); !c)
            return c.error();
        TextRequest tr;
        tr.prompt = prompt;
        tr.max_tokens = max_tokens;
        tr.temperature = 0.0f;
        tr.seed = plan.seed;
        tr.apply_chat_template = false;
        const auto t0 = clock_type::now();
        auto r = target.generate(tr, token);
        const double wall = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
        if (!r)
            return r.error();
        Sample s = r.value();
        s.wall_ms = wall;
        if (s.decode_tokens == 0)
            return make_error(ErrorCode::BackendError, "the generation produced no tokens");
        return s;
    }
};

struct SeriesStats {
    std::vector<double> ttft, ptps, dtps, wall;
    Sample last;
};

// warm-up generations (discarded) then `iterations` measured ones.
Result<SeriesStats> measure_series(Runner& run, const std::string& prompt, std::uint32_t max_tokens,
                                   const std::string& label) {
    for (std::uint32_t i = 0; i < run.plan.warmup; ++i) {
        auto w = run.one_generation(prompt, max_tokens);
        if (!w)
            return w.error();
        run.step(label + " warm-up");
    }
    SeriesStats ss;
    for (std::uint32_t i = 0; i < run.plan.iterations; ++i) {
        auto s = run.one_generation(prompt, max_tokens);
        if (!s)
            return s.error();
        ss.ttft.push_back(s.value().ttft_ms);
        ss.ptps.push_back(s.value().prompt_tps);
        ss.dtps.push_back(s.value().decode_tps);
        ss.wall.push_back(s.value().wall_ms);
        ss.last = s.value();
        run.step(label + " " + std::to_string(i + 1) + "/" + std::to_string(run.plan.iterations));
    }
    return ss;
}

json sweep_entry(const SeriesStats& ss, std::uint32_t target_tokens, std::uint32_t max_tokens, bool decode,
                 std::uint32_t warmup, std::uint32_t iterations) {
    json e;
    e["cold_or_warm"] = "warm";
    e["target_prompt_tokens"] = target_tokens;
    e["prompt_tokens"] = ss.last.prompt_tokens;  // measured by the engine
    e["max_new_tokens"] = max_tokens;
    e["warmup_iterations"] = warmup;
    e["measured_iterations"] = iterations;
    e["ttft_ms_median"] = median(ss.ttft);
    e["ttft_ms_p90"] = percentile(ss.ttft, 0.9);
    e["prompt_tokens_per_second_median"] = median(ss.ptps);
    e["prompt_tokens_per_second_p90"] = percentile(ss.ptps, 0.9);
    e["wall_ms_median"] = median(ss.wall);
    json samples = {{"ttft_ms", stats(ss.ttft)},
                    {"prompt_tokens_per_second", stats(ss.ptps)},
                    {"wall_ms", stats(ss.wall)}};
    if (decode) {  // a 1-token generation is not a decode rate, so only the decode sweep reports one
        e["decode_tokens"] = ss.last.decode_tokens;
        e["decode_tokens_per_second_median"] = median(ss.dtps);
        e["decode_tokens_per_second_p90"] = percentile(ss.dtps, 0.9);
        samples["decode_tokens_per_second"] = stats(ss.dtps);
    }
    e["samples"] = std::move(samples);
    e["result_status"] = "PASS";
    return e;
}

}  // namespace

std::string_view to_string(Suite s) noexcept {
    return s == Suite::Full ? "full" : "quick";
}

double percentile(std::vector<double> v, double p) {
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

std::string prompt_with_sentences(std::uint32_t sentences) {
    std::string s;
    for (std::uint32_t i = 0; i < sentences; ++i) s += kSentence;
    s += "Continue the story in two sentences.";
    return s;
}

std::string base_prompt() {
    return prompt_with_sentences(12);
}

Result<void> validate_plan(const Plan& p) {
    if (p.max_tokens < 1 || p.max_tokens > 4096)
        return make_error(ErrorCode::InvalidArgument, "max_tokens must be 1..4096");
    if (p.warmup > 20)
        return make_error(ErrorCode::InvalidArgument, "warmup must be 0..20");
    if (p.iterations < 1 || p.iterations > 50)
        return make_error(ErrorCode::InvalidArgument, "iterations must be 1..50");
    if (p.suite == Suite::Full) {
        if (p.sweep_sizes.empty() || p.sweep_sizes.size() > kMaxSweepPoints)
            return make_error(ErrorCode::InvalidArgument,
                              "the full suite needs 1.." + std::to_string(kMaxSweepPoints) + " sweep sizes");
        for (const auto n : p.sweep_sizes)
            if (n < kMinSweepTokens || n > kMaxSweepTokens)
                return make_error(ErrorCode::InvalidArgument,
                                  "sweep sizes must be " + std::to_string(kMinSweepTokens) + ".." +
                                      std::to_string(kMaxSweepTokens) + " tokens");
    }
    return {};
}

Result<json> run_suite(IBenchTarget& target, const LoadRequest& load, const Plan& plan, const MetaFn& meta,
                       const CancellationToken& token, const ProgressFn& progress) {
    if (auto v = validate_plan(plan); !v)
        return v.error();

    Runner run{target, load, plan, token, progress};
    const bool full = plan.suite == Suite::Full;
    {
        // load + cold generation + warm-up + measured + warm reload cycles (+ both sweeps)
        double total = 2.0 + plan.warmup + 2.0 * plan.iterations;
        if (full)
            total += 2.0 * static_cast<double>(plan.sweep_sizes.size()) * (plan.warmup + plan.iterations);
        run.steps_total = total;
    }
    bool loaded_by_us = false;
    auto cleanup_on_failure = [&] {
        if (plan.unload_at_end && loaded_by_us)
            (void)target.unload();
    };
    auto fail = [&](const Error& e) -> Result<json> {
        cleanup_on_failure();
        return e;
    };

    // ---- cold: first load + first generation of this run -------------------------------------------
    const std::string prompt = base_prompt();
    auto cold_load = run.timed_load();
    if (!cold_load)
        return cold_load.error();  // nothing was loaded
    loaded_by_us = true;
    run.step("cold load");
    const EngineMetrics after_load = target.metrics();
    auto cold = run.one_generation(prompt, plan.max_tokens);
    if (!cold)
        return fail(cold.error());
    run.step("cold generation");
    const ProcessMemory mem_after_cold = process_memory();

    // ---- warm: discarded warm-up, then measured iterations -------------------------------------------
    auto warm = measure_series(run, prompt, plan.max_tokens, "warm");
    if (!warm)
        return fail(warm.error());
    std::vector<double> warm_loads;
    for (std::uint32_t i = 0; i < plan.iterations; ++i) {
        if (auto c = run.check_cancel(); !c)
            return fail(c.error());
        if (auto r = target.unload(); !r) {
            loaded_by_us = false;
            return r.error();
        }
        loaded_by_us = false;
        auto ms = run.timed_load();
        if (!ms)
            return ms.error();
        loaded_by_us = true;
        warm_loads.push_back(ms.value());
        run.step("warm reload " + std::to_string(i + 1) + "/" + std::to_string(plan.iterations));
    }

    // ---- full suite: prefill and decode sweeps -------------------------------------------------------
    json prefill = json::array(), decode = json::array();
    bool any_failed = false;
    if (full) {
        const double tps_sentence =
            cold.value().prompt_tokens > 0
                ? std::max(1.0, static_cast<double>(cold.value().prompt_tokens) / 12.5)
                : kDefaultTokensPerSentence;
        auto point = [&](std::uint32_t n, std::uint32_t max_new, bool is_decode) -> Result<json> {
            json e;
            const std::uint64_t need = std::uint64_t{n} + max_new + kContextMargin;
            if (need > load.context_tokens) {
                e["target_prompt_tokens"] = n;
                e["max_new_tokens"] = max_new;
                e["result_status"] = "SKIPPED";
                e["reason"] = "prompt of about " + std::to_string(n) + " tokens plus " +
                              std::to_string(max_new) + " new tokens does not fit the context of " +
                              std::to_string(load.context_tokens) + " tokens";
                run.steps_done += static_cast<double>(plan.warmup + plan.iterations);
                return e;
            }
            const auto sentences =
                static_cast<std::uint32_t>(std::max(1.0, std::ceil(static_cast<double>(n) / tps_sentence)));
            auto ss =
                measure_series(run, prompt_with_sentences(sentences), max_new,
                               std::string(is_decode ? "decode" : "prefill") + " sweep " + std::to_string(n));
            if (!ss) {
                if (ss.error().code == ErrorCode::Cancelled)
                    return ss.error();
                any_failed = true;
                e["target_prompt_tokens"] = n;
                e["max_new_tokens"] = max_new;
                e["result_status"] = "FAIL";
                e["error"] = {{"code", std::string(to_string(ss.error().code))},
                              {"message", ss.error().message}};
                return e;
            }
            return sweep_entry(ss.value(), n, max_new, is_decode, plan.warmup, plan.iterations);
        };
        for (const auto n : plan.sweep_sizes) {
            auto e = point(n, 1, false);
            if (!e)
                return fail(e.error());
            prefill.push_back(std::move(e.value()));
        }
        for (const auto n : plan.sweep_sizes) {
            auto e = point(n, plan.max_tokens, true);
            if (!e)
                return fail(e.error());
            decode.push_back(std::move(e.value()));
        }
    }
    const ProcessMemory mem_end = process_memory();
    if (plan.unload_at_end)
        (void)target.unload();

    // ---- machine description (after the measurements so it cannot disturb them) ----------------------
    const Meta m = meta ? meta() : Meta{};
    auto sv = [&](const char* k) -> json {
        return m.summary.is_object() && m.summary.contains(k) ? m.summary[k] : json("unknown");
    };

    std::string backend_name = "unknown";
    if (after_load.backend == "cpu")
        backend_name = "CPU";
    else if (after_load.backend == "vulkan")
        backend_name = (after_load.offloaded_layers >= 0 && after_load.total_layers > 0 &&
                        after_load.offloaded_layers < after_load.total_layers)
                           ? "Hybrid"
                           : "Vulkan";

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
        j["model_sha256"] = m.model_sha256;
        j["model_file"] = m.model_file;
        j["model_size_bytes"] = m.model_size_bytes;
        j["model_format"] = "GGUF";
        j["quantization"] = m.quantization;
        j["quantization_source"] = m.quantization_source;
        j["context_tokens"] = load.context_tokens;
        j["backend"] = backend_name;
        j["load_mode"] = load.load_mode.empty() ? "auto" : load.load_mode;
        j["device"] = after_load.device;
        j["offloaded_layers"] = after_load.offloaded_layers;
        j["total_layers"] = after_load.total_layers;
        j["storage_source"] = m.storage_source;
        j["hdd_read_mib_s_median"] = "unknown";
        j["decompression_mib_s_median"] = "not_applicable";
        j["file_cache"] = "not_controlled";
        j["max_new_tokens"] = plan.max_tokens;
        j["seed"] = plan.seed;
        j["temperature"] = 0.0;
        return j;
    };

    const Sample& c = cold.value();
    json cold_rec = base();
    cold_rec["cold_or_warm"] = "cold";
    cold_rec["warmup_iterations"] = 0;
    cold_rec["measured_iterations"] = 1;
    cold_rec["load_ms_median"] = cold_load.value();
    cold_rec["load_ms_p90"] = cold_load.value();
    cold_rec["ttft_ms_median"] = c.ttft_ms;
    cold_rec["ttft_ms_p90"] = c.ttft_ms;
    cold_rec["prompt_tokens"] = c.prompt_tokens;
    cold_rec["decode_tokens"] = c.decode_tokens;
    cold_rec["prompt_tokens_per_second_median"] = c.prompt_tps;
    cold_rec["decode_tokens_per_second_median"] = c.decode_tps;
    if (mem_after_cold.known) {
        cold_rec["peak_ram_bytes"] = mem_after_cold.peak_working_set;
        cold_rec["peak_commit_bytes"] = mem_after_cold.peak_commit;
    } else {
        cold_rec["peak_ram_bytes"] = "unknown";
    }
    cold_rec["result_status"] = "PASS";
    cold_rec["note"] = m.cold_note;

    const SeriesStats& w = warm.value();
    json rec = base();
    rec["cold_or_warm"] = "warm";
    rec["warmup_iterations"] = plan.warmup;
    rec["measured_iterations"] = plan.iterations;
    rec["load_ms_median"] = median(warm_loads);
    rec["load_ms_p90"] = percentile(warm_loads, 0.9);
    rec["ttft_ms_median"] = median(w.ttft);
    rec["ttft_ms_p90"] = percentile(w.ttft, 0.9);
    rec["prompt_tokens"] = w.last.prompt_tokens;
    rec["decode_tokens"] = w.last.decode_tokens;
    rec["prompt_tokens_per_second_median"] = median(w.ptps);
    rec["prompt_tokens_per_second_p90"] = percentile(w.ptps, 0.9);
    rec["decode_tokens_per_second_median"] = median(w.dtps);
    rec["decode_tokens_per_second_p90"] = percentile(w.dtps, 0.9);
    rec["wall_ms_median"] = median(w.wall);
    if (mem_end.known) {
        rec["peak_ram_bytes"] = mem_end.peak_working_set;
        rec["peak_commit_bytes"] = mem_end.peak_commit;
    } else {
        rec["peak_ram_bytes"] = "unknown";
    }
    rec["peak_ram_note"] =
        "process peak working set since start (includes the cold phase and the warm reloads)";
    rec["samples"] = {{"ttft_ms", stats(w.ttft)},
                      {"prompt_tokens_per_second", stats(w.ptps)},
                      {"decode_tokens_per_second", stats(w.dtps)},
                      {"load_ms", stats(warm_loads)},
                      {"wall_ms", stats(w.wall)}};
    rec["result_status"] = any_failed ? "PARTIAL" : "PASS";
    rec["suite"] = std::string(to_string(plan.suite));
    rec["benchmark_id"] = load.backend == Backend::Cpu ? "B0-text" : "B1-text";
    if (full) {
        rec["prefill_sweep"] = std::move(prefill);
        rec["decode_sweep"] = std::move(decode);
        rec["sweep_note"] =
            "prompts are sized by repeating a fixed sentence to approximate target_prompt_tokens; "
            "prompt_tokens is the count the engine measured. Sweeps vary the prompt fill of one loaded "
            "context (context_tokens); the prefill sweep generates 1 token (TTFT and prompt tokens/s only).";
    }
    rec["cold"] = std::move(cold_rec);
    rec["hcsinfer_version"] = HCS_VERSION_STRING;
    return rec;
}

}  // namespace hcs::bench
