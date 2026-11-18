#pragma once
// Shared benchmark runner (AGENTS.md 21). One implementation serves `HCSInfer benchmark` and
// `POST /hcs/v1/benchmarks`: callers only supply an IBenchTarget (how to load/unload/generate on a real
// engine) and receive the AGENTS.md 21.1 JSON. Nothing is estimated here: every number is either measured
// by the target/runner or the string "unknown" / "not_applicable".
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "hcs/engine.hpp"
#include "hcs/job_queue.hpp"
#include "hcs/result.hpp"

namespace hcs::bench {

enum class Suite { Quick, Full };

std::string_view to_string(Suite s) noexcept;

// One measured generation, as reported by the engine (never filled in by the runner).
struct Sample {
    double ttft_ms = 0.0;
    double prompt_tps = 0.0;
    double decode_tps = 0.0;
    std::uint64_t prompt_tokens = 0;
    std::uint64_t decode_tokens = 0;
    double wall_ms = 0.0;  // measured by the runner around IBenchTarget::generate
};

// The seam to a real engine. Implemented by the CLI (llama adapter) and by the control plane (API job).
class IBenchTarget {
public:
    virtual ~IBenchTarget() = default;
    virtual Result<void> load(const LoadRequest& request, const CancellationToken& token) = 0;
    virtual Result<void> unload() = 0;
    virtual Result<Sample> generate(const TextRequest& request, const CancellationToken& token) = 0;
    virtual EngineMetrics metrics() const = 0;
};

struct Plan {
    Suite suite = Suite::Quick;
    std::uint32_t max_tokens = 64;
    std::uint32_t warmup = 1;
    std::uint32_t iterations = 3;
    std::uint64_t seed = 42;
    // Full suite only: approximate prompt sizes (tokens) of the prefill and decode sweeps. The real token
    // count of every point is measured and recorded; a point that cannot fit the loaded context is recorded
    // as SKIPPED with the reason, never silently dropped.
    std::vector<std::uint32_t> sweep_sizes{128, 512, 1024, 2048};
    bool unload_at_end = true;  // CLI: true. API job: false (leaves the benchmarked model loaded).
};

// Static description of the run, gathered by the caller AFTER the measurements (so that e.g. a doctor report
// or a SHA-256 over the model cannot disturb them).
struct Meta {
    nlohmann::json summary = nlohmann::json::object();  // doctor "summary" object (may be empty)
    std::string storage_source = "unknown";
    std::string model_sha256 = "unknown";
    std::string model_file = "unknown";
    nlohmann::json model_size_bytes = "unknown";
    std::string quantization = "unknown";
    std::string quantization_source = "unknown";
    // Only the CLI starts a fresh process per benchmark; the API job runs inside a long-lived server.
    std::string cold_note = "process-cold: first load and first generation, OS file cache not flushed";
};

using MetaFn = std::function<Meta()>;
using ProgressFn = std::function<void(double fraction, const std::string& message)>;

// Median / nearest-rank percentile / min / max / raw samples.
double median(std::vector<double> v);
double percentile(std::vector<double> v, double p);  // p in (0,1]
nlohmann::json stats(const std::vector<double>& v);

// Fixed benchmark prompt; `sentences` repetitions plus a closing instruction (never logged by the callers).
std::string prompt_with_sentences(std::uint32_t sentences);
std::string base_prompt();  // the 12-sentence prompt of the quick suite

// Validates a Plan against the public bounds (used by CLI and API so both enforce the same limits).
Result<void> validate_plan(const Plan& plan);

// Runs the suite. `load` carries the resolved model/backend/context. Cancellation is checked between samples
// and passed to the target. Returns the AGENTS.md 21.1 document (schema_version 1).
Result<nlohmann::json> run_suite(IBenchTarget& target, const LoadRequest& load, const Plan& plan,
                                 const MetaFn& meta, const CancellationToken& token,
                                 const ProgressFn& progress = {});

}  // namespace hcs::bench
