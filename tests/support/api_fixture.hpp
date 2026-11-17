#pragma once
// Shared fixture for the API/MCP contract tests: a registry with one scanned fake GGUF inside an
// approved root, a FakeTextEngine and a ControlPlane.
#include <memory>

#include "fake_text_engine.hpp"
#include "hcs/api/control_plane.hpp"
#include "hcs/logger.hpp"
#include "temp_dir.hpp"

namespace hcs::test {

struct ApiFixture {
    TempDir dir;
    std::filesystem::path root;
    std::ostringstream log_sink;  // keeps test output quiet and lets tests inspect log content
    Logger logger{&log_sink, LogLevel::Debug};
    ModelRegistry registry;
    PathGuard guard;
    MemoryBudget budget{0};
    std::unique_ptr<JobQueue> queue;
    FakeTextEngine engine;
    std::unique_ptr<api::ControlPlane> cp;

    explicit ApiFixture(bool with_engine = true, api::ControlPlaneOptions opts = {}) {
        root = dir.path() / "models";
        write_file(root / "tiny.gguf", fake_gguf());
        write_file(root / "other.gguf", fake_gguf());
        (void)guard.add_root(root);
        (void)registry.scan(root, guard);
        queue = std::make_unique<JobQueue>();
        opts.version = "test";
        cp = std::make_unique<api::ControlPlane>(registry, guard, budget, *queue,
                                                 with_engine ? &engine : nullptr, logger, opts);
    }
    ~ApiFixture() { queue->shutdown(); }
};

}  // namespace hcs::test
