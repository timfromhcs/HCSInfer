#pragma once
// Minimal control-plane facade used by the HTTP API and the MCP server (AGENTS.md 3.2: no
// transport talks to an engine directly). It ties together ModelRegistry, PathGuard,
// MemoryBudget, JobQueue and an ITextEngine and enforces:
//   * load/unload/generate are serialised through one engine mutex,
//   * model files only load from approved roots (PathGuard) - never from request-supplied paths,
//   * every model transition follows the lifecycle state machine and rolls back on failure,
//   * every long operation is a cancellable job.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "hcs/api/image_stream.hpp"
#include "hcs/api/text_stream.hpp"
#include "hcs/api/vision_stream.hpp"
#include "hcs/benchmark.hpp"
#include "hcs/job_queue.hpp"
#include "hcs/logger.hpp"
#include "hcs/lora.hpp"
#include "hcs/memory_budget.hpp"
#include "hcs/model_registry.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/scheduler/admission.hpp"
#include "hcs/upscale.hpp"

namespace hcs::api {

struct ChatMessage {
    std::string role;  // system | user | assistant | tool (tool: content is the wrapped tool response)
    std::string content;
    // Encoded images (png/jpeg/gif/bmp) attached to a user message (OpenAI image_url data URIs). V1: at most
    // one image per request, single user turn (optionally preceded by a system message). See validate_chat().
    std::vector<ImageInput> images;
};

struct ChatParams {
    std::string model;  // empty => currently loaded model
    std::vector<ChatMessage> messages;
    std::uint32_t max_tokens = 256;
    float temperature = 0.8f;
    std::optional<std::uint64_t> seed;
    // Tool calling (OpenAI `tools`): the request is rendered as ChatML by the control plane (the engine's
    // legacy template path cannot express tools); `tool` role messages are allowed only when set.
    bool tools = false;
};

// Text-to-image (`init_image` empty) or img2img/edit (`init_image` set) request. Bounds are
// enforced by validate_image() before the engine is touched.
struct ImageParams {
    std::string model;  // empty => currently loaded model
    std::string prompt;
    std::string negative_prompt;
    std::uint32_t width = 512;
    std::uint32_t height = 512;
    std::uint32_t steps = 20;
    std::optional<std::uint64_t> seed;
    float strength = 0.75f;
    float cfg_scale = 0.0f;          // 0 => engine default
    std::string preset;              // "" | "fast" | "quality" (replaces steps/cfg, model-family specific)
    bool allow_low_quality = false;  // opt in to sizes/steps below the family quality envelope
    std::optional<ImageInput> init_image;  // edit / img2img
    // 0..kMaxLorasPerRequest LoRAs from the library (hcs/lora.hpp); ids only - paths never come from a
    // caller.
    std::vector<LoraLibrary::Ref> loras;
    // "" | "off" | "4k": after rendering, upscale to a 3840 px long edge (4x model pass + Lanczos, tiled).
    std::string upscale;
};

// Benchmark job request. Bounds are the shared bench::validate_plan() bounds.
struct BenchmarkParams {
    std::string model;  // registered text model id (required)
    bench::Suite suite = bench::Suite::Quick;
    std::uint32_t max_tokens = 64;
    std::uint32_t warmup = 1;
    std::uint32_t iterations = 3;
    std::optional<std::uint64_t> seed;
    std::vector<std::uint32_t> sweep_sizes;  // full suite only; empty => the runner's defaults
};

struct ModelView {
    ModelManifest manifest;
    ModelState state = ModelState::Unregistered;
};

// Thread-safe sink for job events: the queue worker pushes, a request thread pops.
class JobChannel {
public:
    enum class Pop { Event, Done, Timeout };
    void push(const JobEvent& e);
    // Drains pending events first; Done only once the job is terminal AND the queue is empty.
    Pop pop(JobEvent& out, std::chrono::steady_clock::time_point deadline);
    bool done() const;

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<JobEvent> q_;
    bool done_ = false;
};

struct Submitted {
    JobId id;
    std::shared_ptr<JobChannel> channel;
};

struct ControlPlaneOptions {
    std::string version = "unknown";
    // "auto": single-turn (optional system + one user message) uses the engine/model chat template,
    // anything else is rendered as ChatML and sent verbatim. "chatml"/"plain" always render here.
    std::string chat_template = "auto";
    std::uint32_t max_tokens_cap = 4096;
    std::size_t max_prompt_bytes = std::size_t{256} * 1024;
    std::size_t max_messages = 128;
    Backend backend = Backend::Auto;
    std::uint32_t context_tokens = 4096;
    // Image request bounds (untrusted input; AGENTS.md 19.3).
    std::size_t max_image_bytes = 32u << 20;     // encoded prompt/init image
    std::uint64_t max_image_pixels = 16u << 20;  // decoded init image
    std::uint32_t max_image_steps = 150;
    // Output bounds for generated images (a request above them is rejected before the engine runs).
    std::uint32_t max_generate_dimension = 2048;
    std::uint64_t max_generate_pixels = 1u << 20;  // 1 MP: SD1.5-class models on an iGPU/APU
    // Optional: device/driver report (e.g. from hcs_hardware). Called lazily, cached.
    std::function<nlohmann::json()> devices_provider;
    // Optional live memory measurement (e.g. hcs::hardware::measure_memory). When set, the memory budget is
    // re-derived from it before every model load (total = measured headroom + what this process has reserved)
    // so loads are checked against the machine as it is NOW, not as it was at start-up (M10 audit D4;
    // AGENTS.md 10.1). Returning nullopt keeps the previous budget.
    std::function<std::optional<MemoryMeasurements>()> memory_probe;
    // Optional overload controller (plan F1): admission control for model loads and every job, the kill
    // switch and the watchdog. Null = no admission control (the pre-F1 behaviour). The control plane
    // registers its shed handler on it and removes the handler again in its destructor.
    std::shared_ptr<scheduler::AdmissionController> admission;
    // Optional opt-in Model Vault stage (M5 Stage B): called with the resolved model file, the manifest
    // digest (empty when the model was never hashed) and the load's cancellation token, AFTER the budget
    // check and the Loading transition and BEFORE the engine opens the file. It reads the file once through
    // the bounded asynchronous reader and returns a JSON report (bytes, sha256, verified, wall_seconds,
    // mib_per_second, buffer_bytes). An error (digest mismatch, I/O, cancel) aborts the load before the
    // engine sees the file. Empty => the engine loads the path directly (the default).
    std::function<Result<nlohmann::json>(const std::filesystem::path&, const std::string&,
                                         const CancellationToken&)>
        model_prefetch;
};

class ControlPlane {
public:
    // `engine`/`image_engine` may be null (no such backend linked): load and the matching
    // generation calls then fail with Unsupported instead of pretending to work.
    ControlPlane(ModelRegistry& registry, PathGuard& guard, MemoryBudget& budget, JobQueue& queue,
                 ITextEngine* engine, Logger& logger, ControlPlaneOptions options = {},
                 IImageEngine* image_engine = nullptr);
    ~ControlPlane();  // detaches the shed handler and joins a still-running shed unload
    ControlPlane(const ControlPlane&) = delete;
    ControlPlane& operator=(const ControlPlane&) = delete;

    // --- models ---------------------------------------------------------------------------
    std::vector<ModelView> models() const;
    Result<ModelView> model(const std::string& id) const;
    std::optional<std::string> loaded_model() const;
    Result<JobId> submit_load(const std::string& id);  // async job "model.load"
    // Synchronous (used inside jobs / MCP). `token` is checked between validation, budget and engine start: a
    // cancelled load never evicts the loaded model and rolls its lifecycle/budget back (an engine load
    // already running cannot be interrupted - the adapters expose no abort hook yet).
    Result<void> load_model(const std::string& id, const CancellationToken& token = {});
    Result<void> unload_model();  // cancels active generation first
    bool has_engine() const noexcept { return engine_ != nullptr; }
    bool has_stream_engine() const noexcept { return stream_ != nullptr; }
    bool has_image_engine() const noexcept { return image_engine_ != nullptr; }
    bool has_image_stream_engine() const noexcept { return image_stream_ != nullptr; }

    // --- chat -----------------------------------------------------------------------------
    Result<void> validate_chat(const ChatParams& p) const;
    Result<Submitted> submit_chat(const ChatParams& p);
    static std::string build_prompt(const std::vector<ChatMessage>& messages, const std::string& tmpl);
    Result<TextResult> chat_result(JobId id) const;  // NotFound until the job produced one
    // --- vision: chat requests whose messages carry an image take this path inside submit_chat() ------
    static bool chat_has_images(const ChatParams& p);
    bool has_vision_engine() const noexcept;  // the linked text engine implements IVisionStreamEngine
    bool loaded_has_vision() const;           // the loaded model was loaded together with its mmproj
    // The mmproj paired with a registered model ("" = none), resolved through the PathGuard. Pairing: the
    // mmproj's name minus "mmproj"/quantisation tokens equals the model's, else it is the only
    // `architecture == "clip"` file next to the only other model in the same folder.
    Result<std::string> projector_path_for(const ModelManifest& m) const;

    // --- image ----------------------------------------------------------------------------
    // `require_init` selects edit semantics (an init_image is mandatory).
    Result<void> validate_image(const ImageParams& p, bool require_init) const;
    Result<Submitted> submit_image(const ImageParams& p, bool require_init);
    Result<ImageResult> image_result(JobId id) const;  // NotFound until the job produced one

    // --- image LoRA library and upscaler (both optional; absent => the matching request field is
    // Unsupported) --- Scans `dir` (inside an approved root) into the library served to image requests.
    // Replaces the previous library; a failed scan keeps the old one.
    Result<void> scan_loras(const std::filesystem::path& dir, bool verify_hashes = false);
    // {"directory", "note",
    // "loras":[{id,name,base_family,trigger_words,recommended_weight,licence,source,sha256,
    //  sha256_status,size_bytes,status,problems}]}. Never exposes absolute file paths.
    nlohmann::json loras_json() const;
    bool has_lora_library() const;
    // The 4x ONNX upscaler behind `upscale: "4k"`. The control plane owns a shared reference.
    void set_upscaler(std::shared_ptr<upscale::IUpscaleEngine> engine);
    bool has_upscaler() const noexcept { return upscaler_ != nullptr; }

    // --- benchmark (POST /hcs/v1/benchmarks) ---------------------------------------------------
    // Runs the shared benchmark runner (hcs/benchmark.hpp) against a registered text model as a cancellable
    // job "benchmark.run". The job holds the engine for its whole duration (the measurement needs an idle
    // engine), loads/unloads through the normal lifecycle and budget rules, and leaves the benchmarked model
    // loaded. The finished document is retrievable via job_json() ("result") / benchmark_result().
    Result<void> validate_benchmark(const BenchmarkParams& p) const;
    Result<JobId> submit_benchmark(const BenchmarkParams& p);
    Result<nlohmann::json> benchmark_result(JobId id) const;  // NotFound until the job produced one

    // --- jobs -----------------------------------------------------------------------------
    static std::optional<JobId> parse_job_id(const std::string& text);
    Result<JobStatus> job_status(JobId id) const;
    Result<void> cancel_job(JobId id);
    nlohmann::json job_json(const JobStatus& st) const;
    bool wait_job(JobId id, std::chrono::milliseconds t) const { return queue_.wait(id, t); }

    // --- overload protection (plan F1) -------------------------------------------------------------
    // Kill switch: cancels every queued/running job, then unloads the model, all inside the configured
    // deadline (an engine load that cannot be interrupted is reported as unload_pending and finishes in the
    // background; ~ControlPlane joins it). Works without a configured controller too. Never throws.
    scheduler::ShedReport shed(const std::string& reason);
    bool has_admission() const noexcept { return opt_.admission != nullptr; }
    int retry_after_seconds() const noexcept;  // Retry-After hint for 429/503 answers
    nlohmann::json overload_json() const;      // {"enabled": false} without a controller

    // --- introspection ---------------------------------------------------------------------
    nlohmann::json runtime_json() const;
    nlohmann::json devices_json() const;
    nlohmann::json memory_json() const;
    nlohmann::json model_json(const ModelView& v) const;

    const PathGuard& guard() const noexcept { return guard_; }
    const ControlPlaneOptions& options() const noexcept { return opt_; }
    ModelRegistry& registry() noexcept { return registry_; }

private:
    Result<void> validate_vision_chat(const ChatParams& p) const;
    Result<Submitted> submit_vision_chat(const ChatParams& p);
    Result<void> load_locked(const std::string& id, const CancellationToken& token = {});
    // Re-derives the budget total from opt_.memory_probe: measured headroom + what this process has reserved.
    void refresh_budget() const;
    // Heuristic KV-cache size for a text model (the engine reports no figure before load): per-token bytes
    // scale with the model size, clamped to 96..512 KiB, times the context. Deliberately an upper-ish
    // estimate.
    static std::uint64_t kv_cache_estimate(std::uint64_t model_bytes, std::uint32_t context_tokens) noexcept;
    Result<void> unload_locked();
    // Admission ticket for one job; a null pointer (success) when no controller is configured.
    Result<std::shared_ptr<scheduler::Ticket>> admit_job(scheduler::WorkKind kind, const std::string& what,
                                                         std::uint64_t predicted_bytes);
    scheduler::ShedReport shed_impl(const std::string& reason);
    Result<Submitted> submit_with_channel(const std::string& name, JobFn fn);
    // Resolves request LoRA references against the library for `model` (id; "" = the loaded model).
    Result<std::vector<LoraSpec>> resolve_loras(const ImageParams& p) const;
    Result<void> check_upscale_request(const ImageParams& p) const;
    void note_result(JobId id, const TextResult& r);
    void note_image_result(JobId id, const ImageResult& r);
    void note_benchmark_result(JobId id, nlohmann::json doc);
    class BenchTarget;  // IBenchTarget over this control plane (control_plane_benchmark.cpp)

    ModelRegistry& registry_;
    PathGuard& guard_;
    MemoryBudget& budget_;
    JobQueue& queue_;
    ITextEngine* engine_;
    ITextStreamEngine* stream_;
    IImageEngine* image_engine_;
    IImageStreamEngine* image_stream_;
    Logger& log_;
    ControlPlaneOptions opt_;

    std::mutex engine_mutex_;          // serialises engine load/unload/generate
    mutable std::mutex budget_mutex_;  // makes "re-measure + check + reserve" one step w.r.t. memory_json()
    mutable std::mutex state_mutex_;
    std::optional<std::string> loaded_;
    std::optional<std::uint64_t> loaded_bytes_;
    bool loaded_is_image_ = false;              // guarded by state_mutex_
    bool loaded_has_projector_ = false;         // guarded by state_mutex_
    std::optional<nlohmann::json> last_vault_;  // report of the last vault-prefetched load; state_mutex_
    std::atomic<std::uint64_t> active_job_{0};
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    mutable std::mutex results_mutex_;
    std::map<std::uint64_t, TextResult> results_;
    std::map<std::uint64_t, ImageResult> image_results_;
    std::map<std::uint64_t, nlohmann::json> benchmark_results_;
    std::mutex shed_mutex_;                 // guards shed_unload_
    std::shared_future<bool> shed_unload_;  // a kill-switch unload that outlived its deadline
    mutable std::mutex lora_mutex_;
    std::shared_ptr<LoraLibrary> loras_;  // guarded by lora_mutex_ (replaced wholesale by scan_loras)
    std::shared_ptr<upscale::IUpscaleEngine> upscaler_;
    mutable std::mutex devices_mutex_;
    mutable std::optional<nlohmann::json> devices_cache_;
};

}  // namespace hcs::api
