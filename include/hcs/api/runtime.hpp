#pragma once
// Process bootstrap shared by `HCSInfer serve` and `HCSInfer mcp stdio`: loads the registry
// (read-only), builds the approved-root PathGuard, JobQueue, MemoryBudget and ControlPlane.
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hcs/api/control_plane.hpp"
#include "hcs/api/http_api.hpp"
#include "hcs/scheduler/admission.hpp"
#include "hcs/settings.hpp"
#include "hcs/telemetry/resource_probe.hpp"

namespace hcs::api {

struct RuntimeOptions {
    std::filesystem::path state_dir;
    std::vector<std::string> model_roots;        // approved roots in addition to settings.json
    std::unique_ptr<ITextEngine> engine;         // null => make_default_text_engine() (may still be null)
    std::unique_ptr<IImageEngine> image_engine;  // null => make_default_image_engine() (may still be null)
    ControlPlaneOptions control;
    JobQueueOptions queue;
    // Live memory measurement used for the budget. Empty => the hardware module's measurement when it is
    // linked into this build (otherwise the budget stays unknown and /hcs/v1/memory says so).
    std::function<std::optional<MemoryMeasurements>()> memory_probe;
    // Overload protection (plan F1). Always on; nullopt = the defaults (keep >= 2.5 GiB free, commit < 90 %,
    // at most 4 jobs in flight). `enabled = false` in the config switches it off. The probe defaults to the
    // live system probe; tests inject a fake one.
    std::optional<scheduler::AdmissionConfig> admission;
    telemetry::ResourceProbe admission_probe;
};

class Runtime {
public:
    static Result<std::unique_ptr<Runtime>> create(RuntimeOptions options, Logger& logger);

    ModelRegistry registry;
    PathGuard guard;
    MemoryBudget budget;
    std::unique_ptr<JobQueue> queue;         // control-plane jobs (chat/load)
    std::unique_ptr<JobQueue> engine_queue;  // private to the engine adapter (it submits its own jobs)
    std::unique_ptr<ITextEngine> engine;
    std::unique_ptr<IImageEngine> image_engine;
    std::shared_ptr<scheduler::AdmissionController> admission;  // declared before `control`: outlives it
    std::unique_ptr<ControlPlane> control;
    Settings settings;

    // Cancels and joins both job queues BEFORE any member is destroyed (a running job still uses control, the
    // engines and the registry; members are otherwise destroyed in reverse declaration order, control first).
    // Safe to call shutdown() beforehand: queue shutdown is idempotent. Soak defect D-S1.
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

private:
    Runtime() = default;
};

// Engine linked into this binary (llama adapter when built with it), or nullptr.
// `engine_queue` is the engine's private queue and must outlive the engine.
std::unique_ptr<ITextEngine> make_default_text_engine(JobQueue& engine_queue);

// Image engine linked into this binary (stable-diffusion.cpp adapter when built with it), or nullptr.
// It never uses `engine_queue` for the blocking HTTP/MCP path; the queue must still outlive the engine.
std::unique_ptr<IImageEngine> make_default_image_engine(JobQueue& engine_queue);

// Transport budget used by `HCSInfer serve` (soak defect D-S2). Connection workers are cheap blocked threads;
// the real work limit is ApiOptions::max_inflight (HTTP 429), enforced after the request is read. The pool is
// therefore sized well above max_inflight so ~64 idle / half-open local sockets cannot starve GET
// /hcs/v1/health.
TransportOptions serve_transport_options(const ApiOptions& api, const std::string& host, int port);

// Ordered graceful stop used by `serve` on SIGINT/SIGTERM (soak defect D-S3): cancel and join the job queues
// FIRST so a request thread blocked on a running generation returns, then stop the transport (whose stop()
// joins every connection thread and would otherwise wait for the generation to end on its own).
void graceful_shutdown(HttpServer& server, Runtime& runtime);

// `HCSInfer serve ...`. Returns a process exit code (see include/hcs/result.hpp).
int run_serve(const std::vector<std::string>& args, const std::filesystem::path& state_dir, std::ostream& out,
              std::ostream& err);

}  // namespace hcs::api
