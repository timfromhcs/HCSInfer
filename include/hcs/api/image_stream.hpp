#pragma once
// Blocking image execution extension of IImageEngine, owned by the API layer (the same reason as
// hcs/api/text_stream.hpp: IImageEngine::generate() only returns a JobId, so there is no channel
// through which the encoded image can reach a caller). An adapter that serves HTTP/MCP image routes
// implements IImageStreamEngine: it blocks while rendering and stops promptly when the token is
// cancelled.
#include <cstdint>
#include <string>
#include <vector>

#include "hcs/engine.hpp"
#include "hcs/job_queue.hpp"

namespace hcs::api {

struct ImageResult {
    std::string format = "png";
    std::vector<std::uint8_t> bytes;  // encoded image (format above)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t steps = 0;
    double generation_ms = 0.0;
};

class IImageStreamEngine : public IImageEngine {
public:
    // Called with the engine already loaded. `request.init_image` selects img2img/edit semantics.
    virtual Result<ImageResult> generate_image(const ImageRequest& request,
                                               const CancellationToken& token) = 0;
    virtual Result<ImageResult> edit_image(const ImageRequest& request, const CancellationToken& token) = 0;
};

}  // namespace hcs::api
