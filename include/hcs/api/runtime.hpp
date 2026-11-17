#pragma once
// Process bootstrap shared by `HCSInfer serve` and `HCSInfer mcp stdio`: loads the registry
// (read-only), builds the approved-root PathGuard, JobQueue, MemoryBudget and ControlPlane.
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "hcs/api/control_plane.hpp"
#include "hcs/settings.hpp"

namespace hcs::api {

struct RuntimeOptions {
    std::filesystem::path state_dir;
    std::vector<std::string> model_roots;  // approved roots in addition to settings.json
    std::unique_ptr<ITextEngine> engine;   // null => make_default_text_engine() (may still be null)
    ControlPlaneOptions control;
    JobQueueOptions queue;
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
    std::unique_ptr<ControlPlane> control;
    Settings settings;

private:
    Runtime() = default;
};

// Engine linked into this binary (llama adapter when built with it), or nullptr.
// `engine_queue` is the engine's private queue and must outlive the engine.
std::unique_ptr<ITextEngine> make_default_text_engine(JobQueue& engine_queue);

// `HCSInfer serve ...`. Returns a process exit code (see include/hcs/result.hpp).
int run_serve(const std::vector<std::string>& args, const std::filesystem::path& state_dir, std::ostream& out,
              std::ostream& err);

}  // namespace hcs::api
