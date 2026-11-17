// Default image engine for HCSInfer serve / mcp stdio: the stable-diffusion.cpp adapter. Kept in its own
// translation unit so builds without the diffusion module link the null fallback in runtime.cpp instead.
#include <memory>

#include "hcs/api/runtime.hpp"
#include "hcs/engines/diffusion_engine.hpp"

namespace hcs::api {

std::unique_ptr<IImageEngine> make_default_image_engine(JobQueue& engine_queue) {
    return std::make_unique<engines::DiffusionImageEngine>(engine_queue);
}

}  // namespace hcs::api
