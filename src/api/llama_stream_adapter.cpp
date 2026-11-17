// Bridges hcs::engines::LlamaTextEngine (job/event based) to hcs::api::ITextStreamEngine
// (blocking, sink based). Only compiled when the llama engine module is part of the build.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "hcs/api/runtime.hpp"
#include "hcs/api/vision_stream.hpp"
#include "hcs/engines/llama_engine.hpp"

namespace hcs::api {
namespace {

class LlamaStreamEngine final : public ITextStreamEngine, public IVisionStreamEngine {
public:
    explicit LlamaStreamEngine(JobQueue& engine_queue) : queue_(engine_queue), engine_(engine_queue) {}

    Result<void> load(const LoadRequest& r) override { return engine_.load(r); }
    Result<void> unload() override { return engine_.unload(); }
    Result<JobId> generate(const TextRequest& r) override { return engine_.generate(r); }
    Result<JobId> analyze_image(const VisionRequest& r) override { return engine_.analyze_image(r); }
    EngineMetrics metrics() const override { return engine_.metrics(); }

    bool vision_loaded() const override { return engine_.vision_loaded(); }

    Result<TextResult> generate_stream(const TextRequest& req, const TokenSink& sink,
                                       const CancellationToken& token) override {
        return run_stream(
            req.max_tokens, [&](EventCallback cb) { return engine_.generate(req, std::move(cb)); }, sink,
            token);
    }

    Result<TextResult> analyze_image_stream(const VisionRequest& req, const TokenSink& sink,
                                            const CancellationToken& token) override {
        return run_stream(
            req.text.max_tokens, [&](EventCallback cb) { return engine_.analyze_image(req, std::move(cb)); },
            sink, token);
    }

private:
    // Shared blocking driver: submit through `start`, forward Output events to `sink`, honour cancellation.
    template <class Start>
    Result<TextResult> run_stream(std::uint32_t max_tokens, Start&& start, const TokenSink& sink,
                                  const CancellationToken& token) {
        struct Shared {
            std::mutex m;
            std::condition_variable cv;
            std::deque<std::string> pieces;
            bool done = false;
        };
        auto sh = std::make_shared<Shared>();
        auto job = start([sh](const JobEvent& e) {
            std::lock_guard lk(sh->m);
            if (e.kind == JobEventKind::Output)
                sh->pieces.push_back(e.text);
            if (e.kind == JobEventKind::StateChanged && is_terminal(e.state))
                sh->done = true;
            sh->cv.notify_all();
        });
        if (!job)
            return job.error();
        bool stop = false;
        while (true) {
            std::deque<std::string> batch;
            bool done;
            {
                std::unique_lock lk(sh->m);
                sh->cv.wait_for(lk, std::chrono::milliseconds(20),
                                [&] { return sh->done || !sh->pieces.empty(); });
                batch.swap(sh->pieces);
                done = sh->done;
            }
            for (auto& p : batch) {
                if (!stop && !sink(p))
                    stop = true;  // sink gone: stop generating, drop remaining text
            }
            if ((stop || token.cancelled()) && !done) {
                (void)queue_.cancel(job.value());
                stop = true;
            }
            if (done && batch.empty())
                break;
        }
        (void)queue_.wait(job.value(), std::chrono::seconds(5));
        auto st = queue_.status(job.value());
        if (!st)
            return st.error();
        TextResult r;
        const auto m = engine_.metrics();
        r.prompt_tokens = static_cast<std::uint32_t>(m.prompt_tokens);
        r.completion_tokens = static_cast<std::uint32_t>(m.decode_tokens);
        r.ttft_ms = m.ttft_ms;
        r.tokens_per_second = m.tokens_per_second;
        if (st.value().state == JobState::Failed)
            return st.value().error ? *st.value().error
                                    : make_error(ErrorCode::BackendError, "generation failed");
        if (st.value().state == JobState::Cancelled || stop || token.cancelled())
            r.finish_reason = "cancelled";
        else
            r.finish_reason = r.completion_tokens >= max_tokens ? "length" : "stop";
        return r;
    }

    JobQueue& queue_;
    engines::LlamaTextEngine engine_;
};

}  // namespace

std::unique_ptr<ITextEngine> make_default_text_engine(JobQueue& engine_queue) {
    return std::make_unique<LlamaStreamEngine>(engine_queue);
}

}  // namespace hcs::api
