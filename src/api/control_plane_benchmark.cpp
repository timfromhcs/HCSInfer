// POST /hcs/v1/benchmarks backend: the shared benchmark runner (hcs/benchmark.hpp - the same code `HCSInfer
// benchmark` runs) driven through the control plane, so loads honour the memory budget, the lifecycle state
// machine and the engine mutex exactly like chat does.
#include <algorithm>
#include <exception>
#include <filesystem>

#include "hcs/api/control_plane.hpp"

namespace hcs::api {

namespace {
using json = nlohmann::json;
constexpr std::size_t kMaxRetainedBenchmarks = 16;
}  // namespace

// IBenchTarget over the control plane. Every method runs inside the benchmark job with engine_mutex_ held by
// the job (see submit_benchmark), so it calls the *_locked variants.
class ControlPlane::BenchTarget final : public bench::IBenchTarget {
public:
    BenchTarget(ControlPlane& cp, std::string model_id, JobContext& ctx)
        : cp_(cp), id_(std::move(model_id)), ctx_(ctx) {}

    Result<void> load(const LoadRequest&, const CancellationToken& token) override {
        // A cold measurement needs a real load: if the model happens to be resident already, unload it first.
        if (const auto cur = cp_.loaded_model(); cur && *cur == id_) {
            if (auto r = cp_.unload_locked(); !r)
                return r;
        }
        return cp_.load_locked(id_, token);
    }

    Result<void> unload() override { return cp_.unload_locked(); }

    EngineMetrics metrics() const override { return cp_.stream_->metrics(); }

    Result<bench::Sample> generate(const TextRequest& request, const CancellationToken& token) override {
        (void)cp_.registry_.transition(id_, ModelState::Running);
        cp_.active_job_.store(ctx_.id().value);
        Result<TextResult> r = make_error(ErrorCode::Internal, "engine threw");
        try {
            // The generated text is discarded: a benchmark measures speed, never content.
            r = cp_.stream_->generate_stream(
                request, [&token](std::string_view) { return !token.cancelled(); }, token);
        } catch (const std::exception& e) {
            r = error_from_engine_exception(e);
        }
        cp_.active_job_.store(0);
        (void)cp_.registry_.transition(id_, ModelState::Loaded);
        if (!r)
            return r.error();
        if (token.cancelled() || r.value().finish_reason == "cancelled")
            return make_error(ErrorCode::Cancelled, "benchmark cancelled");
        const EngineMetrics m = cp_.stream_->metrics();
        bench::Sample s;
        s.ttft_ms = r.value().ttft_ms;
        s.decode_tps = r.value().tokens_per_second;
        s.prompt_tps = m.prompt_tokens_per_second;  // TextResult has no prompt rate; the adapter's metrics do
        s.prompt_tokens = r.value().prompt_tokens;
        s.decode_tokens = r.value().completion_tokens;
        return s;
    }

private:
    ControlPlane& cp_;
    std::string id_;
    JobContext& ctx_;
};

Result<void> ControlPlane::validate_benchmark(const BenchmarkParams& p) const {
    if (!engine_)
        return make_error(ErrorCode::Unsupported, "no inference engine is built into this binary");
    if (!stream_)
        return make_error(ErrorCode::Unsupported,
                          "the linked text engine does not implement hcs::api::ITextStreamEngine");
    if (!is_valid_model_id(p.model))
        return make_error(ErrorCode::InvalidArgument, "invalid model id");
    auto m = registry_.get(p.model);
    if (!m)
        return m.error();
    if (m.value().kind == ModelKind::Image)
        return make_error(ErrorCode::InvalidArgument,
                          "'" + p.model + "' is an image model; the benchmark runs on text models");
    bench::Plan plan;
    plan.suite = p.suite;
    plan.max_tokens = p.max_tokens;
    plan.warmup = p.warmup;
    plan.iterations = p.iterations;
    if (!p.sweep_sizes.empty())
        plan.sweep_sizes = p.sweep_sizes;
    return bench::validate_plan(plan);
}

void ControlPlane::note_benchmark_result(JobId id, json doc) {
    std::lock_guard lk(results_mutex_);
    benchmark_results_[id.value] = std::move(doc);
    while (benchmark_results_.size() > kMaxRetainedBenchmarks)
        benchmark_results_.erase(benchmark_results_.begin());
}

Result<json> ControlPlane::benchmark_result(JobId id) const {
    std::lock_guard lk(results_mutex_);
    auto it = benchmark_results_.find(id.value);
    if (it == benchmark_results_.end())
        return make_error(ErrorCode::NotFound, "no benchmark result for job");
    return it->second;
}

Result<JobId> ControlPlane::submit_benchmark(const BenchmarkParams& params) {
    if (auto v = validate_benchmark(params); !v)
        return v.error();
    auto ticket = admit_job(scheduler::WorkKind::Benchmark, params.model, 0);
    if (!ticket)
        return ticket.error();
    return queue_.submit(
        "benchmark.run", [this, params, tk = ticket.value()](JobContext& ctx) -> Result<void> {
            (void)tk;  // holds the admission slot until the job closure is destroyed
            if (ctx.cancelled())
                return make_error(ErrorCode::Cancelled, "cancelled before start");
            // The whole run is one critical section: a concurrent chat/load must not slip between samples.
            std::lock_guard lk(engine_mutex_);
            auto mm = registry_.get(params.model);
            if (!mm)
                return mm.error();
            const ModelManifest manifest = mm.value();

            bench::Plan plan;
            plan.suite = params.suite;
            plan.max_tokens = params.max_tokens;
            plan.warmup = params.warmup;
            plan.iterations = params.iterations;
            if (params.seed)
                plan.seed = *params.seed;
            if (!params.sweep_sizes.empty())
                plan.sweep_sizes = params.sweep_sizes;
            plan.unload_at_end = false;  // leave the benchmarked model loaded, like a model.load job would

            LoadRequest lr;
            lr.model_id = manifest.id;
            lr.backend = opt_.backend;
            lr.context_tokens = opt_.context_tokens;

            const bench::MetaFn meta = [this, &manifest]() {
                bench::Meta m;
                const json dev = devices_json();
                if (dev.is_object() && dev.contains("summary") && dev["summary"].is_object())
                    m.summary = dev["summary"];
                m.storage_source = "unknown (not measured by the API benchmark job)";
                m.model_sha256 = manifest.sha256.empty()
                                     ? "unknown (not hashed; run `models scan` with hashing)"
                                     : manifest.sha256;
                m.model_file = std::filesystem::path(manifest.path).filename().string();
                m.model_size_bytes = manifest.size_bytes;
                m.quantization = manifest.quantization;
                m.quantization_source = "model registry manifest";
                m.cold_note =
                    "first load of this benchmark job inside a running server: the engine and graphics "
                    "driver "
                    "were "
                    "already initialised and the OS file cache was not flushed, so this is not a "
                    "process-cold "
                    "figure";
                return m;
            };

            BenchTarget target(*this, manifest.id, ctx);
            const bench::ProgressFn progress = [&ctx](double fraction, const std::string& message) {
                ctx.progress(fraction, message);
            };
            Result<json> doc = make_error(ErrorCode::Internal, "benchmark threw");
            try {
                doc = bench::run_suite(target, lr, plan, meta, ctx.token(), progress);
            } catch (const std::exception& e) {
                doc = error_from_engine_exception(e, "benchmark exception");
            }
            if (!doc) {
                log_.warn("control", "benchmark failed",
                          {{"model", manifest.id}, {"code", std::string(to_string(doc.error().code))}});
                return doc.error();
            }
            note_benchmark_result(ctx.id(), std::move(doc.value()));
            log_.info("control", "benchmark finished", {{"model", manifest.id}});
            return {};
        });
}

}  // namespace hcs::api
