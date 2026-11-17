#pragma once
// Blocking image-understanding extension of ITextEngine, owned by the API layer (same reason as
// hcs/api/text_stream.hpp: ITextEngine::analyze_image() only returns a JobId, so no channel exists
// through which the generated text reaches a caller). An adapter that serves HTTP/MCP image input
// implements IVisionStreamEngine: it blocks inside analyze_image_stream(), pushes each decoded piece
// to `sink` and stops promptly when the sink returns false or the token is cancelled.
#include "hcs/api/text_stream.hpp"

namespace hcs::api {

class IVisionStreamEngine {
public:
    virtual ~IVisionStreamEngine() = default;
    // Called with a model AND its vision projector already loaded; Unsupported otherwise.
    virtual Result<TextResult> analyze_image_stream(const VisionRequest& request, const TokenSink& sink,
                                                    const CancellationToken& token) = 0;
    // True iff the currently loaded model has a working vision projector.
    virtual bool vision_loaded() const = 0;
};

}  // namespace hcs::api
