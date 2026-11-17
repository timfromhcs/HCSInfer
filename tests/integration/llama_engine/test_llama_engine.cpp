// Integration tests for the llama.cpp text adapter. Real model required for most tests:
//   set HCS_TEST_GGUF=<path to a small GGUF>   (optional HCS_TEST_GGUF2 for a second model,
//   HCS_TEST_EVIDENCE_DIR=<dir> to append machine-readable JSON-lines results).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "hcs/engines/llama_engine.hpp"
#include "hcs/job_queue.hpp"
#include "temp_dir.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace hcs;
using namespace hcs::engines;
using namespace std::chrono_literals;

namespace {

std::string env(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

#define REQUIRE_MODEL_OR_SKIP(path_var)                                                              \
    const std::string path_var = env("HCS_TEST_GGUF");                                               \
    if (path_var.empty()) GTEST_SKIP() << "NOT RUN: HCS_TEST_GGUF is not set (real-model test)"

#define REQUIRE_GPU_OR_SKIP()                                                                        \
    if (LlamaTextEngine::gpu_devices().empty())                                                      \
    GTEST_SKIP() << "NOT RUN: no GPU device visible to this build (vulkan_compiled=" +               \
                        std::to_string(LlamaTextEngine::vulkan_compiled()) + ")"

struct GenResult {
    JobState state = JobState::Queued;
    std::string text;
    std::optional<Error> error;
};

GenResult run_text(JobQueue& q, LlamaTextEngine& e, TextRequest req) {
    GenResult r;
    std::mutex m;
    auto id = e.generate(req, [&](const JobEvent& ev) {
        if (ev.kind == JobEventKind::Output) {
            std::lock_guard l(m);
            r.text += ev.text;
        }
    });
    EXPECT_TRUE(id.ok()) << (id.ok() ? "" : id.error().message);
    if (!id.ok()) return r;
    EXPECT_TRUE(q.wait(id.value(), 300s));
    auto st = q.status(id.value());
    r.state = st.value().state;
    r.error = st.value().error;
    return r;
}

LoadRequest load_req(const std::string& path, Backend b, std::uint32_t ctx = 2048) {
    LoadRequest lr;
    lr.model_path = path;
    lr.backend = b;
    lr.context_tokens = ctx;
    return lr;
}

std::uint64_t private_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
#else
    return 0;
#endif
}
std::uint64_t working_set_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
#else
    return 0;
#endif
}

void evidence(const std::string& json_line) {
    std::cout << "[evidence] " << json_line << "\n";
    const std::string dir = env("HCS_TEST_EVIDENCE_DIR");
    if (dir.empty()) return;
    std::ofstream f(dir + "/llama-adapter-results.jsonl", std::ios::app);
    f << json_line << "\n";
}

std::string metrics_json(const char* test, const EngineMetrics& m, const GenResult& r) {
    return std::string("{\"test\":\"") + test + "\",\"backend\":\"" + m.backend + "\",\"device\":\"" + m.device +
           "\",\"offloaded_layers\":" + std::to_string(m.offloaded_layers) +
           ",\"total_layers\":" + std::to_string(m.total_layers) + ",\"load_ms\":" + std::to_string(m.load_ms) +
           ",\"ttft_ms\":" + std::to_string(m.ttft_ms) + ",\"prompt_tokens\":" + std::to_string(m.prompt_tokens) +
           ",\"prompt_tps\":" + std::to_string(m.prompt_tokens_per_second) +
           ",\"decode_tokens\":" + std::to_string(m.decode_tokens) +
           ",\"decode_tps\":" + std::to_string(m.decode_tokens_per_second) +
           ",\"output_chars\":" + std::to_string(r.text.size()) + "}";
}

TextRequest greedy(const std::string& prompt, std::uint32_t max_tokens = 32) {
    TextRequest t;
    t.prompt = prompt;
    t.max_tokens = max_tokens;
    t.temperature = 0.0f;
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Always-run tests: validation, errors, rollback (no real model needed).
// ---------------------------------------------------------------------------------------------
TEST(LlamaEngineErrors, GenerateBeforeLoadIsInvalidState) {
    JobQueue q;
    LlamaTextEngine e(q);
    auto r = e.generate(greedy("hi"));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidState);
    EXPECT_FALSE(e.loaded());
    EXPECT_TRUE(e.unload().ok());  // idempotent
}

TEST(LlamaEngineErrors, MissingFileIsNotFound) {
    JobQueue q;
    LlamaTextEngine e(q);
    auto r = e.load(load_req("Z:/definitely/not/here.gguf", Backend::Cpu));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_FALSE(e.loaded());
}

TEST(LlamaEngineErrors, NonGgufIsModelError) {
    test::TempDir d;
    test::write_file(d / "x.gguf", "this is definitely not a gguf file");
    JobQueue q;
    LlamaTextEngine e(q);
    auto r = e.load(load_req((d / "x.gguf").string(), Backend::Cpu));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::ModelError);
}

TEST(LlamaEngineErrors, TruncatedGgufFailsCleanlyAndEngineStaysUsable) {
    test::TempDir d;
    test::write_file(d / "trunc.gguf", test::fake_gguf());  // valid magic+version, nothing else
    JobQueue q;
    LlamaTextEngine e(q);
    auto r = e.load(load_req((d / "trunc.gguf").string(), Backend::Cpu));
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(r.error().code == ErrorCode::ModelError || r.error().code == ErrorCode::Unsupported)
        << static_cast<int>(r.error().code);
    EXPECT_FALSE(e.loaded());
    EXPECT_FALSE(e.last_load_log().empty()) << "upstream log must be captured for diagnostics";
    // rolled back: a second failing load behaves identically (no stuck "loading" state)
    auto r2 = e.load(load_req((d / "trunc.gguf").string(), Backend::Cpu));
    EXPECT_FALSE(r2.ok());
    EXPECT_NE(r2.error().code, ErrorCode::InvalidState);
}

TEST(LlamaEngineErrors, BadArguments) {
    JobQueue q;
    LlamaTextEngine e(q);
    LoadRequest lr = load_req("x.gguf", Backend::Cpu);
    lr.context_tokens = 0;
    EXPECT_EQ(e.load(lr).error().code, ErrorCode::InvalidArgument);
    lr = load_req("x.gguf", Backend::Cpu);
    lr.load_mode = "bogus";
    EXPECT_EQ(e.load(lr).error().code, ErrorCode::InvalidArgument);
    lr = load_req("", Backend::Cpu);
    EXPECT_EQ(e.load(lr).error().code, ErrorCode::InvalidArgument);
    lr = load_req("x.gguf", Backend::Cpu);
    lr.projector_path = "definitely-missing-mmproj.gguf";
    EXPECT_EQ(e.load(lr).error().code, ErrorCode::NotFound);  // projector is validated before upstream is touched
}

// Vision request validation without any model (detailed vision tests: tests/integration/llama_vision).
TEST(LlamaEngineErrors, VisionRequestValidation) {
    JobQueue q;
    LlamaTextEngine e(q);
    auto r = e.analyze_image(VisionRequest{});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);  // empty image
    VisionRequest v;
    v.image.bytes = {1, 2, 3};
    v.text.prompt = "what is this?";
    auto nm = e.analyze_image(v);
    ASSERT_FALSE(nm.ok());
    EXPECT_EQ(nm.error().code, ErrorCode::InvalidState);  // valid request, but no model loaded
}

#ifndef HCS_TEST_EXPECT_VULKAN_BUILD
TEST(LlamaEngineErrors, VulkanOnCpuOnlyBuildIsUnsupported) {
    test::TempDir d;
    test::write_file(d / "x.gguf", test::fake_gguf());
    JobQueue q;
    LlamaTextEngine e(q);
    auto r = e.load(load_req((d / "x.gguf").string(), Backend::Vulkan));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
}
#endif

// ---------------------------------------------------------------------------------------------
// Real-model tests.
// ---------------------------------------------------------------------------------------------
TEST(LlamaEngineReal, CpuGeneration) {
    REQUIRE_MODEL_OR_SKIP(model);
    JobQueue q;
    LlamaTextEngine e(q);
    auto lr = e.load(load_req(model, Backend::Cpu));
    ASSERT_TRUE(lr.ok()) << lr.error().message;
    auto m0 = e.metrics();
    EXPECT_EQ(m0.backend, "cpu");
    EXPECT_EQ(m0.offloaded_layers, 0);
    EXPECT_GT(m0.load_ms, 0.0);
    EXPECT_GT(m0.resident_bytes, 0u);
    auto r = run_text(q, e, greedy("Reply with exactly: HCSInfer smoke test passed.", 48));
    ASSERT_EQ(r.state, JobState::Succeeded) << (r.error ? r.error->message : "");
    EXPECT_FALSE(r.text.empty());
    auto m = e.metrics();
    EXPECT_GT(m.prompt_tokens, 5u);
    EXPECT_GT(m.decode_tokens_per_second, 0.0);
    EXPECT_GT(m.ttft_ms, 0.0);
    EXPECT_EQ(m.backend, "cpu");
    // CPU run must not mention a GPU offload of layers
    EXPECT_EQ(e.last_load_log().find("offloaded 29/29"), std::string::npos);
    evidence(metrics_json("CpuGeneration", m, r));
    std::cout << "[output] " << r.text << "\n";
}

TEST(LlamaEngineReal, VulkanGeneration) {
    REQUIRE_MODEL_OR_SKIP(model);
    REQUIRE_GPU_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    auto lr = e.load(load_req(model, Backend::Vulkan));
    ASSERT_TRUE(lr.ok()) << lr.error().message;
    auto m0 = e.metrics();
    EXPECT_EQ(m0.backend, "vulkan");
    EXPECT_GT(m0.offloaded_layers, 0) << "no layers offloaded; log:\n" << e.last_load_log();
    EXPECT_EQ(m0.offloaded_layers, m0.total_layers + 1) << "expected all layers + output layer offloaded";
    // Backend identity evidence from the upstream log
    EXPECT_NE(e.last_load_log().find("Vulkan"), std::string::npos);
    std::cout << "[backend-identity] device=" << m0.device << " offloaded=" << m0.offloaded_layers << "/"
              << m0.total_layers << "\n";
    auto r = run_text(q, e, greedy("Reply with exactly: HCSInfer smoke test passed.", 48));
    ASSERT_EQ(r.state, JobState::Succeeded) << (r.error ? r.error->message : "");
    EXPECT_FALSE(r.text.empty());
    auto m = e.metrics();
    EXPECT_GT(m.decode_tokens_per_second, 0.0);
    evidence(metrics_json("VulkanGeneration", m, r));
    std::cout << "[output] " << r.text << "\n";
}

TEST(LlamaEngineReal, GreedyIsDeterministicOnCpu) {
    REQUIRE_MODEL_OR_SKIP(model);
    JobQueue q;
    LlamaTextEngine e(q);
    ASSERT_TRUE(e.load(load_req(model, Backend::Cpu)).ok());
    auto a = run_text(q, e, greedy("Name three colors.", 24));
    auto b = run_text(q, e, greedy("Name three colors.", 24));
    ASSERT_EQ(a.state, JobState::Succeeded);
    ASSERT_EQ(b.state, JobState::Succeeded);
    EXPECT_EQ(a.text, b.text) << "greedy decoding must be repeatable (KV cache reset between requests)";
}

TEST(LlamaEngineReal, PromptTooLongForContextIsRejectedCleanly) {
    REQUIRE_MODEL_OR_SKIP(model);
    JobQueue q;
    LlamaTextEngine e(q);
    ASSERT_TRUE(e.load(load_req(model, Backend::Cpu, 64)).ok());
    auto r = run_text(q, e, greedy(std::string(2000, 'a') + " b c d e f", 8));
    EXPECT_EQ(r.state, JobState::Failed);
    ASSERT_TRUE(r.error.has_value());
    EXPECT_EQ(r.error->code, ErrorCode::InvalidArgument);
    // engine still usable
    auto ok = run_text(q, e, greedy("Hi", 4));
    EXPECT_EQ(ok.state, JobState::Succeeded);
}

static void cancellation_case(const std::string& model, Backend b, const char* name) {
    JobQueue q;
    LlamaTextEngine e(q);
    auto lr = e.load(load_req(model, b, 4096));
    ASSERT_TRUE(lr.ok()) << lr.error().message;

    std::atomic<int> chunks{0};
    auto id = e.generate(greedy("Write a very long story about a dragon, with many chapters.", 3000),
                         [&](const JobEvent& ev) {
                             if (ev.kind == JobEventKind::Output) chunks.fetch_add(1);
                         });
    ASSERT_TRUE(id.ok());
    const auto t0 = std::chrono::steady_clock::now();
    while (chunks.load() < 5 && std::chrono::steady_clock::now() - t0 < 120s) std::this_thread::sleep_for(10ms);
    ASSERT_GE(chunks.load(), 5) << "generation did not start streaming";
    const int at_cancel = chunks.load();
    const auto t_cancel = std::chrono::steady_clock::now();
    ASSERT_TRUE(q.cancel(id.value()).ok());
    ASSERT_TRUE(q.wait(id.value(), 30s));
    const double stop_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_cancel).count();
    auto st = q.status(id.value()).value();
    EXPECT_EQ(st.state, JobState::Cancelled);
    EXPECT_LT(chunks.load(), 3000) << "generation ran to the token limit despite cancellation";
    EXPECT_LT(stop_ms, 5000.0);
    evidence(std::string("{\"test\":\"") + name + "\",\"chunks_at_cancel\":" + std::to_string(at_cancel) +
             ",\"chunks_final\":" + std::to_string(chunks.load()) + ",\"stop_latency_ms\":" + std::to_string(stop_ms) +
             "}");

    // The context is intact and reusable after a cancelled job.
    auto r = run_text(q, e, greedy("Say hi.", 8));
    EXPECT_EQ(r.state, JobState::Succeeded);
    // Cancel of a *queued* job and unload-with-active-job both leave nothing behind.
    auto busy = e.generate(greedy("Count to one thousand slowly.", 3000));
    ASSERT_TRUE(busy.ok());
    auto queued = e.generate(greedy("Second job.", 8));
    ASSERT_TRUE(queued.ok());
    ASSERT_TRUE(e.unload().ok());  // cancels both, waits, frees
    EXPECT_FALSE(e.loaded());
    EXPECT_TRUE(is_terminal(q.status(busy.value()).value().state));
    EXPECT_TRUE(is_terminal(q.status(queued.value()).value().state));
    EXPECT_EQ(q.status(queued.value()).value().state, JobState::Cancelled);
}

TEST(LlamaEngineReal, CancellationMidGenerationCpu) {
    REQUIRE_MODEL_OR_SKIP(model);
    cancellation_case(model, Backend::Cpu, "CancelCpu");
}

TEST(LlamaEngineReal, CancellationMidGenerationVulkan) {
    REQUIRE_MODEL_OR_SKIP(model);
    REQUIRE_GPU_OR_SKIP();
    cancellation_case(model, Backend::Vulkan, "CancelVulkan");
}

static void cycle_case(const std::string& model, Backend b, const char* name, int cycles) {
    JobQueue q;
    LlamaTextEngine e(q);
    std::vector<std::uint64_t> priv, ws;
    for (int i = 0; i < cycles; ++i) {
        auto lr = e.load(load_req(model, b, 1024));
        ASSERT_TRUE(lr.ok()) << "cycle " << i << ": " << lr.error().message;
        auto r = run_text(q, e, greedy("Hi", 4));
        ASSERT_EQ(r.state, JobState::Succeeded) << "cycle " << i;
        ASSERT_TRUE(e.unload().ok());
        ASSERT_FALSE(e.loaded());
        priv.push_back(private_bytes());
        ws.push_back(working_set_bytes());
    }
    std::string pj = "[", wj = "[";
    for (std::size_t i = 0; i < priv.size(); ++i) {
        pj += (i ? "," : "") + std::to_string(priv[i]);
        wj += (i ? "," : "") + std::to_string(ws[i]);
    }
    pj += "]";
    wj += "]";
    // Leak criterion: after warm-up (3 cycles: driver/shader caches, allocator pools) private bytes may not
    // keep growing. Allow 48 MiB slack across the remaining cycles for allocator noise.
    const std::int64_t growth = static_cast<std::int64_t>(priv.back()) - static_cast<std::int64_t>(priv[2]);
    const std::int64_t growth_ws = static_cast<std::int64_t>(ws.back()) - static_cast<std::int64_t>(ws[2]);
    evidence(std::string("{\"test\":\"") + name + "\",\"cycles\":" + std::to_string(cycles) +
             ",\"private_bytes_after_each_unload\":" + pj + ",\"working_set_after_each_unload\":" + wj +
             ",\"private_growth_after_cycle3\":" + std::to_string(growth) +
             ",\"working_set_growth_after_cycle3\":" + std::to_string(growth_ws) + "}");
    EXPECT_LT(growth, 48ll * 1024 * 1024) << "private bytes grew by " << growth << " over cycles 3.." << cycles;
}

TEST(LlamaEngineReal, LoadUnloadTenTimesCpuNoLeakGrowth) {
    REQUIRE_MODEL_OR_SKIP(model);
    cycle_case(model, Backend::Cpu, "Cycle10Cpu", 10);
}

TEST(LlamaEngineReal, LoadUnloadTenTimesVulkanNoLeakGrowth) {
    REQUIRE_MODEL_OR_SKIP(model);
    REQUIRE_GPU_OR_SKIP();
    cycle_case(model, Backend::Vulkan, "Cycle10Vulkan", 10);
}

TEST(LlamaEngineReal, ModelSwitchBetweenBackendsAndModels) {
    REQUIRE_MODEL_OR_SKIP(model);
    const std::string model2 = env("HCS_TEST_GGUF2").empty() ? model : env("HCS_TEST_GGUF2");
    JobQueue q;
    LlamaTextEngine e(q);

    ASSERT_TRUE(e.load(load_req(model, Backend::Cpu)).ok());
    EXPECT_EQ(e.load(load_req(model2, Backend::Cpu)).error().code, ErrorCode::InvalidState)
        << "loading over a loaded model must be refused, not leak";
    EXPECT_EQ(run_text(q, e, greedy("Hi", 4)).state, JobState::Succeeded);
    ASSERT_TRUE(e.unload().ok());

    if (!LlamaTextEngine::gpu_devices().empty()) {
        ASSERT_TRUE(e.load(load_req(model2, Backend::Vulkan)).ok());
        EXPECT_EQ(e.metrics().backend, "vulkan");
        EXPECT_EQ(run_text(q, e, greedy("Hi", 4)).state, JobState::Succeeded);
        ASSERT_TRUE(e.unload().ok());
    }
    ASSERT_TRUE(e.load(load_req(model, Backend::Auto, 1024)).ok());
    evidence("{\"test\":\"ModelSwitch\",\"auto_backend\":\"" + e.metrics().backend +
             "\",\"second_model_distinct\":" + (model2 != model ? "true" : "false") + "}");
    EXPECT_EQ(run_text(q, e, greedy("Hi", 4)).state, JobState::Succeeded);
}

TEST(LlamaEngineReal, LoadModesAreAccepted) {
    REQUIRE_MODEL_OR_SKIP(model);
    JobQueue q;
    LlamaTextEngine e(q);
    for (const char* mode : {"none", "mmap"}) {
        LoadRequest lr = load_req(model, Backend::Cpu, 512);
        lr.load_mode = mode;
        auto r = e.load(lr);
        ASSERT_TRUE(r.ok()) << mode << ": " << r.error().message;
        EXPECT_EQ(run_text(q, e, greedy("Hi", 3)).state, JobState::Succeeded) << mode;
        ASSERT_TRUE(e.unload().ok());
    }
}
