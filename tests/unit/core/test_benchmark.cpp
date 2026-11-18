// Unit tests for the shared benchmark runner. A scripted IBenchTarget feeds KNOWN inputs so the tests verify
// the arithmetic (median/p90/raw samples), the AGENTS.md 21.1 document shape, call accounting, skipping and
// cancellation. The scripted values are test inputs, not benchmark results (AGENTS.md 16.2).
#include <gtest/gtest.h>

#include <atomic>
#include <functional>

#include "hcs/benchmark.hpp"

using namespace hcs;
using namespace hcs::bench;
using nlohmann::json;

namespace {

class ScriptedTarget final : public IBenchTarget {
public:
    int loads = 0, unloads = 0, generations = 0;
    bool fail_load = false;
    std::uint64_t decode_tokens = 8;
    std::uint64_t bytes_per_token = 4;  // prompt_tokens = prompt.size() / 4 (test rule)
    std::function<Result<Sample>(int call, const TextRequest&)> hook;
    std::vector<std::uint32_t> max_tokens_seen;

    Result<void> load(const LoadRequest&, const CancellationToken&) override {
        if (fail_load)
            return make_error(ErrorCode::ModelError, "scripted load failure");
        ++loads;
        return {};
    }
    Result<void> unload() override {
        ++unloads;
        return {};
    }
    Result<Sample> generate(const TextRequest& r, const CancellationToken&) override {
        const int call = ++generations;
        max_tokens_seen.push_back(r.max_tokens);
        if (hook)
            return hook(call, r);
        Sample s;
        s.ttft_ms = 10.0 * call;
        s.prompt_tps = 100.0 + call;
        s.decode_tps = 20.0 + call;
        s.prompt_tokens = r.prompt.size() / bytes_per_token;
        s.decode_tokens = decode_tokens;
        return s;
    }
    EngineMetrics metrics() const override {
        EngineMetrics m;
        m.backend = "cpu";
        m.device = "scripted";
        return m;
    }
};

LoadRequest load_request(std::uint32_t ctx = 4096) {
    LoadRequest lr;
    lr.model_id = "tiny";
    lr.model_path = "tiny-Q4_K_M.gguf";
    lr.context_tokens = ctx;
    lr.backend = Backend::Cpu;
    return lr;
}

Meta meta_fn_value() {
    Meta m;
    m.summary = {{"cpu", "test-cpu"}, {"os_build", "test-os"}};
    m.model_sha256 = "abc";
    m.model_file = "tiny-Q4_K_M.gguf";
    m.model_size_bytes = 123;
    m.quantization = "Q4_K_M";
    m.quantization_source = "file name";
    return m;
}

Result<json> run(ScriptedTarget& t, Plan p, std::uint32_t ctx = 4096, CancellationToken tok = {}) {
    return run_suite(t, load_request(ctx), p, [] { return meta_fn_value(); }, tok);
}

}  // namespace

TEST(BenchStats, MedianPercentileAndStatsShape) {
    EXPECT_DOUBLE_EQ(median({3, 1, 2}), 2.0);
    EXPECT_DOUBLE_EQ(median({4, 1, 3, 2}), 2.5);
    EXPECT_DOUBLE_EQ(median({}), 0.0);
    // nearest rank: p90 of 10 values is the 9th smallest
    std::vector<double> ten{10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    EXPECT_DOUBLE_EQ(percentile(ten, 0.9), 9.0);
    EXPECT_DOUBLE_EQ(percentile({5}, 0.9), 5.0);
    const json s = stats({3, 1, 2});
    EXPECT_DOUBLE_EQ(s["median"], 2.0);
    EXPECT_DOUBLE_EQ(s["min"], 1.0);
    EXPECT_DOUBLE_EQ(s["max"], 3.0);
    ASSERT_EQ(s["samples"].size(), 3u);
    EXPECT_DOUBLE_EQ(s["samples"][0], 3.0);  // raw order is preserved
}

TEST(BenchPlan, ValidationBounds) {
    Plan p;
    EXPECT_TRUE(validate_plan(p));
    p.iterations = 0;
    EXPECT_EQ(validate_plan(p).error().code, ErrorCode::InvalidArgument);
    p = {};
    p.iterations = 51;
    EXPECT_FALSE(validate_plan(p));
    p = {};
    p.warmup = 21;
    EXPECT_FALSE(validate_plan(p));
    p = {};
    p.max_tokens = 0;
    EXPECT_FALSE(validate_plan(p));
    p.max_tokens = 4097;
    EXPECT_FALSE(validate_plan(p));
    p = {};
    p.sweep_sizes = {};
    p.suite = Suite::Full;
    EXPECT_FALSE(validate_plan(p));  // a full suite needs at least one sweep point
    p.sweep_sizes = {16, 16, 16, 16, 16, 16, 16, 16, 16};
    EXPECT_FALSE(validate_plan(p));  // more than 8 points
    p.sweep_sizes = {15};
    EXPECT_FALSE(validate_plan(p));  // below 16 tokens
    p.sweep_sizes = {1048577};
    EXPECT_FALSE(validate_plan(p));
}

TEST(BenchQuick, DocumentShapeAndMeasuredStatistics) {
    ScriptedTarget t;
    Plan p;
    p.warmup = 1;
    p.iterations = 3;
    auto r = run(t, p);
    ASSERT_TRUE(r) << r.error().message;
    const json& j = r.value();

    for (const char* k : {"schema_version",
                          "timestamp_utc",
                          "git_commit",
                          "build_type",
                          "compiler",
                          "cmake_version",
                          "os_build",
                          "cpu",
                          "gpu",
                          "gpu_vendor_id",
                          "gpu_device_id",
                          "driver_version",
                          "vulkan_api_version",
                          "ram_bytes",
                          "model_sha256",
                          "model_format",
                          "quantization",
                          "context_tokens",
                          "backend",
                          "storage_source",
                          "cold_or_warm",
                          "warmup_iterations",
                          "measured_iterations",
                          "ttft_ms_median",
                          "ttft_ms_p90",
                          "prompt_tokens_per_second_median",
                          "decode_tokens_per_second_median",
                          "hdd_read_mib_s_median",
                          "decompression_mib_s_median",
                          "peak_ram_bytes",
                          "result_status",
                          "samples",
                          "cold",
                          "suite",
                          "benchmark_id",
                          "load_ms_median",
                          "load_ms_p90"}) {
        EXPECT_TRUE(j.contains(k)) << "missing key " << k;
    }
    EXPECT_EQ(j["schema_version"], 1);
    EXPECT_EQ(j["cold_or_warm"], "warm");
    EXPECT_EQ(j["suite"], "quick");
    EXPECT_EQ(j["result_status"], "PASS");
    EXPECT_EQ(j["benchmark_id"], "B0-text");
    EXPECT_EQ(j["cpu"], "test-cpu");
    EXPECT_EQ(j["gpu"], "unknown");  // absent in the summary => "unknown", never guessed
    EXPECT_EQ(j["measured_iterations"], 3);
    EXPECT_EQ(j["warmup_iterations"], 1);
    EXPECT_EQ(j["cold"]["cold_or_warm"], "cold");
    EXPECT_EQ(j["cold"]["measured_iterations"], 1);
    EXPECT_FALSE(j.contains("prefill_sweep"));
    EXPECT_FALSE(j.contains("decode_sweep"));

    // Call order: cold (call 1), warm-up (call 2), measured calls 3,4,5 => ttft 30,40,50; median 40.
    EXPECT_DOUBLE_EQ(j["cold"]["ttft_ms_median"], 10.0);
    EXPECT_DOUBLE_EQ(j["ttft_ms_median"], 40.0);
    EXPECT_DOUBLE_EQ(j["ttft_ms_p90"], 50.0);
    ASSERT_EQ(j["samples"]["ttft_ms"]["samples"].size(), 3u);  // raw samples kept
    EXPECT_DOUBLE_EQ(j["samples"]["ttft_ms"]["samples"][0], 30.0);
    EXPECT_DOUBLE_EQ(j["samples"]["decode_tokens_per_second"]["median"], 24.0);
    EXPECT_EQ(j["samples"]["load_ms"]["samples"].size(), 3u);

    // 1 cold load + 3 warm reloads; 1 cold + 1 warm-up + 3 measured generations; unloaded at the end.
    EXPECT_EQ(t.loads, 4);
    EXPECT_EQ(t.generations, 5);
    EXPECT_EQ(t.unloads, 3 + 1);
}

TEST(BenchQuick, UnloadAtEndCanBeDisabled) {
    ScriptedTarget t;
    Plan p;
    p.iterations = 2;
    p.unload_at_end = false;
    ASSERT_TRUE(run(t, p));
    EXPECT_EQ(t.unloads, 2);  // only the warm reload cycles
}

TEST(BenchQuick, ColdNoteComesFromMeta) {
    ScriptedTarget t;
    Plan p;
    p.iterations = 1;
    auto r = run_suite(t, load_request(), p,
                       [] {
                           Meta m = meta_fn_value();
                           m.cold_note = "first load of this benchmark job";
                           return m;
                       },
                       {});
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value()["cold"]["note"], "first load of this benchmark job");
}

TEST(BenchQuick, LoadFailureAndZeroTokenGenerationAreErrors) {
    ScriptedTarget bad_load;
    bad_load.fail_load = true;
    auto a = run(bad_load, {});
    ASSERT_FALSE(a);
    EXPECT_EQ(a.error().code, ErrorCode::ModelError);

    ScriptedTarget none;
    none.decode_tokens = 0;
    auto b = run(none, {});
    ASSERT_FALSE(b);
    EXPECT_EQ(b.error().code, ErrorCode::BackendError);
    EXPECT_GE(none.unloads, 1);  // cleaned up after the failure
}

TEST(BenchQuick, InvalidPlanIsRejectedBeforeTouchingTheTarget) {
    ScriptedTarget t;
    Plan p;
    p.iterations = 0;
    auto r = run(t, p);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(t.loads, 0);
}

TEST(BenchQuick, CancellationStopsBetweenSamplesAndUnloads) {
    ScriptedTarget t;
    CancellationSource src;
    t.hook = [&](int call, const TextRequest&) -> Result<Sample> {
        if (call == 2)
            src.request_cancel();  // cancel while the warm-up runs
        Sample s;
        s.decode_tokens = 4;
        return s;
    };
    Plan p;
    p.iterations = 5;
    auto r = run(t, p, 4096, src.token());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(t.generations, 2);  // no further generation after the cancel
    EXPECT_GE(t.unloads, 1);
}

TEST(BenchFull, SweepsAreMeasuredPerSizeAndCarryRawSamples) {
    ScriptedTarget t;
    Plan p;
    p.suite = Suite::Full;
    p.warmup = 1;
    p.iterations = 2;
    p.max_tokens = 16;
    p.sweep_sizes = {64, 256};
    auto r = run(t, p);
    ASSERT_TRUE(r) << r.error().message;
    const json& j = r.value();
    EXPECT_EQ(j["suite"], "full");
    ASSERT_TRUE(j.contains("prefill_sweep"));
    ASSERT_TRUE(j.contains("decode_sweep"));
    ASSERT_EQ(j["prefill_sweep"].size(), 2u);
    ASSERT_EQ(j["decode_sweep"].size(), 2u);
    for (const char* sweep : {"prefill_sweep", "decode_sweep"}) {
        for (const auto& e : j[sweep]) {
            EXPECT_EQ(e["result_status"], "PASS");
            EXPECT_EQ(e["cold_or_warm"], "warm");
            EXPECT_EQ(e["measured_iterations"], 2);
            EXPECT_EQ(e["warmup_iterations"], 1);
            EXPECT_TRUE(e.contains("target_prompt_tokens"));
            EXPECT_GT(e["prompt_tokens"].get<std::uint64_t>(), 0u);  // measured, from the target
            EXPECT_TRUE(e.contains("ttft_ms_median"));
            EXPECT_TRUE(e.contains("ttft_ms_p90"));
            EXPECT_TRUE(e.contains("prompt_tokens_per_second_median"));
            ASSERT_EQ(e["samples"]["ttft_ms"]["samples"].size(), 2u);
        }
    }
    EXPECT_EQ(j["prefill_sweep"][0]["target_prompt_tokens"], 64);
    EXPECT_EQ(j["prefill_sweep"][1]["target_prompt_tokens"], 256);
    EXPECT_EQ(j["prefill_sweep"][0]["max_new_tokens"], 1);
    EXPECT_EQ(j["decode_sweep"][0]["max_new_tokens"], 16);
    EXPECT_TRUE(j["decode_sweep"][0].contains("decode_tokens_per_second_median"));
    EXPECT_TRUE(j["decode_sweep"][0].contains("decode_tokens_per_second_p90"));
    EXPECT_FALSE(j["prefill_sweep"][0].contains("decode_tokens_per_second_median"));  // 1 token: not a rate

    // quick part (5 generations: cold+warmup+3?) iterations=2 => cold + 1 + 2 = 4; sweeps: 4 points x (1+2).
    EXPECT_EQ(t.generations, 4 + 4 * 3);
    // larger prompts really are larger
    EXPECT_GT(j["prefill_sweep"][1]["prompt_tokens"].get<std::uint64_t>(),
              j["prefill_sweep"][0]["prompt_tokens"].get<std::uint64_t>());
    EXPECT_EQ(j["result_status"], "PASS");
}

TEST(BenchFull, PointsThatDoNotFitTheContextAreSkippedWithAReason) {
    ScriptedTarget t;
    Plan p;
    p.suite = Suite::Full;
    p.iterations = 1;
    p.warmup = 0;
    p.max_tokens = 16;
    p.sweep_sizes = {64, 100000};
    auto r = run(t, p, 1024);
    ASSERT_TRUE(r) << r.error().message;
    const json& j = r.value();
    ASSERT_EQ(j["prefill_sweep"].size(), 2u);
    EXPECT_EQ(j["prefill_sweep"][0]["result_status"], "PASS");
    EXPECT_EQ(j["prefill_sweep"][1]["result_status"], "SKIPPED");
    EXPECT_NE(j["prefill_sweep"][1]["reason"].get<std::string>().find("context"), std::string::npos);
    EXPECT_FALSE(j["prefill_sweep"][1].contains("ttft_ms_median"));  // nothing invented for a skipped point
    EXPECT_EQ(j["decode_sweep"][1]["result_status"], "SKIPPED");
    EXPECT_EQ(j["result_status"], "PASS");  // a skip is expected, not a failure
}

TEST(BenchFull, AFailingSweepPointIsRecordedAndMakesTheRunPartial) {
    ScriptedTarget t;
    t.hook = [&](int, const TextRequest& r) -> Result<Sample> {
        if (r.prompt.size() > 1500)
            return make_error(ErrorCode::BackendError, "scripted overflow");
        Sample s;
        s.prompt_tokens = r.prompt.size() / 4;
        s.decode_tokens = 2;
        s.ttft_ms = 1;
        return s;
    };
    Plan p;
    p.suite = Suite::Full;
    p.iterations = 1;
    p.warmup = 0;
    p.sweep_sizes = {16, 512};
    auto r = run(t, p);
    ASSERT_TRUE(r) << r.error().message;
    const json& j = r.value();
    EXPECT_EQ(j["prefill_sweep"][0]["result_status"], "PASS");
    EXPECT_EQ(j["prefill_sweep"][1]["result_status"], "FAIL");
    EXPECT_TRUE(j["prefill_sweep"][1].contains("error"));
    EXPECT_EQ(j["result_status"], "PARTIAL");
}

TEST(BenchFull, CancellationInsideASweepAborts) {
    ScriptedTarget t;
    CancellationSource src;
    t.hook = [&](int call, const TextRequest& r) -> Result<Sample> {
        if (call == 3)
            src.request_cancel();
        Sample s;
        s.prompt_tokens = r.prompt.size() / 4;
        s.decode_tokens = 2;
        return s;
    };
    Plan p;
    p.suite = Suite::Full;
    p.iterations = 1;
    p.warmup = 0;
    p.sweep_sizes = {16, 32};
    auto r = run(t, p, 4096, src.token());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
}

TEST(BenchPrompt, SizedPromptsGrowAndAreDeterministic) {
    EXPECT_EQ(prompt_with_sentences(12), base_prompt());
    EXPECT_GT(prompt_with_sentences(40).size(), prompt_with_sentences(10).size());
    EXPECT_EQ(prompt_with_sentences(7), prompt_with_sentences(7));
}
