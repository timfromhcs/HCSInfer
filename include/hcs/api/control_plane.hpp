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
#include <functional>
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
#include "hcs/job_queue.hpp"
#include "hcs/logger.hpp"
#include "hcs/memory_budget.hpp"
#include "hcs/model_registry.hpp"
#include "hcs/path_guard.hpp"

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
    float cfg_scale = 0.0f;                // 0 => engine default
    std::string preset;                    // "" | "fast" | "quality" (replaces steps/cfg, model-family specific)
    std::optional<ImageInput> init_image;  // edit / img2img
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
};

class ControlPlane {
public:
    // `engine`/`image_engine` may be null (no such backend linked): load and the matching
    // generation calls then fail with Unsupported instead of pretending to work.
    ControlPlane(ModelRegistry& registry, PathGuard& guard, MemoryBudget& budget, JobQueue& queue,
                 ITextEngine* engine, Logger& logger, ControlPlaneOptions options = {},
                 IImageEngine* image_engine = nullptr);

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

    // --- jobs -----------------------------------------------------------------------------
    static std::optional<JobId> parse_job_id(const std::string& text);
    Result<JobStatus> job_status(JobId id) const;
    Result<void> cancel_job(JobId id);
    nlohmann::json job_json(const JobStatus& st) const;
    bool wait_job(JobId id, std::chrono::milliseconds t) const { return queue_.wait(id, t); }

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
    Result<Submitted> submit_with_channel(const std::string& name, JobFn fn);
    void note_result(JobId id, const TextResult& r);
    void note_image_result(JobId id, const ImageResult& r);

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
    bool loaded_is_image_ = false;       // guarded by state_mutex_
    bool loaded_has_projector_ = false;  // guarded by state_mutex_
    std::atomic<std::uint64_t> active_job_{0};
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    mutable std::mutex results_mutex_;
    std::map<std::uint64_t, TextResult> results_;
    std::map<std::uint64_t, ImageResult> image_results_;
    mutable std::mutex devices_mutex_;
    mutable std::optional<nlohmann::json> devices_cache_;
};

}  // namespace hcs::api
