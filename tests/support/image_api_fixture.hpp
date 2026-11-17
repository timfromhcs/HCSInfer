#pragma once
// Fixture for the image API/MCP contract tests: one text model ("tiny") and one registered image model
// ("sdmodel") inside an approved root, a FakeTextEngine, a FakeImageEngine and a ControlPlane wired with
// both.
#include <memory>

#include "fake_image_engine.hpp"
#include "fake_text_engine.hpp"
#include "hcs/api/control_plane.hpp"
#include "hcs/logger.hpp"
#include "temp_dir.hpp"

namespace hcs::test {

struct ImageApiFixture {
    TempDir dir;
    std::filesystem::path root;
    std::ostringstream log_sink;
    Logger logger{&log_sink, LogLevel::Debug};
    ModelRegistry registry;
    PathGuard guard;
    MemoryBudget budget{0};
    std::unique_ptr<JobQueue> queue;
    FakeTextEngine text;
    FakeImageEngine image;
    std::unique_ptr<api::ControlPlane> cp;

    explicit ImageApiFixture(bool with_image_engine = true, api::ControlPlaneOptions opts = {}) {
        root = dir.path() / "models";
        write_file(root / "tiny.gguf", fake_gguf());
        (void)guard.add_root(root);
        (void)registry.scan(root, guard);
        write_file(root / "sd-weights.gguf",
                   fake_gguf());  // written after the scan: registered by hand below
        // The scan cannot tell a header-only fake from an image model: register the kind explicitly.
        ModelManifest m;
        m.id = "sdmodel";
        m.display_name = "sdmodel";
        m.path = (root / "sd-weights.gguf").string();
        m.format = ModelFormat::Gguf;
        m.kind = ModelKind::Image;
        m.size_bytes = 64;
        (void)registry.add(m);
        queue = std::make_unique<JobQueue>();
        opts.version = "test";
        cp = std::make_unique<api::ControlPlane>(
            registry, guard, budget, *queue, &text, logger, opts,
            with_image_engine ? static_cast<IImageEngine*>(&image) : nullptr);
    }
    ~ImageApiFixture() { queue->shutdown(); }
};

}  // namespace hcs::test
