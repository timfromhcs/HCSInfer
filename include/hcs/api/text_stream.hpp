#pragma once
// Streaming extension of ITextEngine, owned by the API layer (include/hcs/engine.hpp has no
// channel through which generated text could reach a caller: generate() only returns a JobId).
// An engine adapter that wants to serve HTTP/MCP chat implements ITextStreamEngine: it blocks
// inside generate_stream(), pushes each decoded piece to `sink`, and stops promptly when the
// sink returns false or the token is cancelled.
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "hcs/engine.hpp"
#include "hcs/job_queue.hpp"

namespace hcs::api {

using TokenSink = std::function<bool(std::string_view piece)>;  // return false => stop generating

struct TextResult {
    std::string finish_reason = "stop";  // "stop" | "length" | "cancelled"
    std::uint32_t prompt_tokens = 0;
    std::uint32_t completion_tokens = 0;
    double ttft_ms = 0.0;
    double tokens_per_second = 0.0;
};

class ITextStreamEngine : public ITextEngine {
public:
    // Called with the engine already loaded. Must not retain `sink`/`token` after returning.
    virtual Result<TextResult> generate_stream(const TextRequest& request, const TokenSink& sink,
                                               const CancellationToken& token) = 0;
};

}  // namespace hcs::api
