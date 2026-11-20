#pragma once
// Deterministic FakeTextEngine for CONTRACT tests only (AGENTS.md 16.2): it exercises the
// API/MCP plumbing (auth, streaming, cancellation, lifecycle). It is NOT evidence of inference.
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "hcs/api/text_stream.hpp"

namespace hcs::test {

class FakeTextEngine final : public api::ITextStreamEngine {
public:
    std::vector<std::string> pieces{"Hello", " from", " the", " fake", " engine"};
    int delay_ms = 0;               // per piece
    bool run_until_cancel = false;  // endless "tok" stream, only stops on cancel/sink==false
    bool fail_load = false;
    // Fault injection for the OOM / shutdown tests (plumbing only):
    ErrorCode fail_load_code = ErrorCode::ModelError;  // code returned when fail_load is set
    bool throw_bad_alloc_on_load = false;              // an engine whose allocator threw
    bool throw_bad_alloc_on_generate = false;
    int load_delay_ms = 0;  // load blocks this long (a "slow weight read")
    std::atomic<bool> loading{false};
    std::atomic<int> loads{0}, unloads{0};
    std::atomic<bool> generating{false};
    std::mutex mu;
    std::string last_path, last_prompt, last_system;
    bool last_apply_template = true;

    Result<void> load(const LoadRequest& r) override {
        loading = true;
        struct Reset {
            std::atomic<bool>& f;
            ~Reset() { f = false; }
        } reset{loading};
        if (load_delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(load_delay_ms));
        if (throw_bad_alloc_on_load)
            throw std::bad_alloc();
        if (fail_load)
            return make_error(fail_load_code, "fake load failure");
        std::lock_guard lk(mu);
        last_path = r.model_path;
        ++loads;
        loaded_ = true;
        return {};
    }
    Result<void> unload() override {
        ++unloads;
        loaded_ = false;
        return {};
    }
    Result<JobId> generate(const TextRequest&) override {
        return make_error(ErrorCode::Unsupported, "fake: use generate_stream");
    }
    Result<JobId> analyze_image(const VisionRequest&) override {
        return make_error(ErrorCode::Unsupported, "fake: no vision");
    }
    EngineMetrics metrics() const override {
        EngineMetrics m;
        m.backend = "fake";
        m.device = "none";
        return m;
    }

    Result<api::TextResult> generate_stream(const TextRequest& req, const api::TokenSink& sink,
                                            const CancellationToken& token) override {
        {
            std::lock_guard lk(mu);
            last_prompt = req.prompt;
            last_system = req.system_prompt;
            last_apply_template = req.apply_chat_template;
        }
        if (throw_bad_alloc_on_generate)
            throw std::bad_alloc();
        generating = true;
        api::TextResult r;
        auto step = [&](const std::string& p) {
            if (delay_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            if (token.cancelled())
                return false;
            if (!sink(p))
                return false;
            ++r.completion_tokens;
            return true;
        };
        bool completed = true;
        if (run_until_cancel) {
            while (step("tok ")) {
            }
            completed = false;
        } else {
            for (const auto& p : pieces) {
                if (!step(p)) {
                    completed = false;
                    break;
                }
            }
        }
        generating = false;
        r.prompt_tokens = 7;
        r.finish_reason = completed ? "stop" : "cancelled";
        return r;
    }

private:
    std::atomic<bool> loaded_{false};
};

}  // namespace hcs::test
