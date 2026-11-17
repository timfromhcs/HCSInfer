// Real-model integration tests for image understanding (llama.cpp libmtmd through LlamaTextEngine).
//   HCS_TEST_VLM=<multimodal GGUF>  HCS_TEST_MMPROJ=<its mmproj GGUF>   (required for the real tests)
//   HCS_TEST_GGUF=<plain text GGUF>                                     (mismatched-projector test)
//   HCS_TEST_EVIDENCE_DIR=<dir>  appends JSON-lines results.
// The test image is generated in-process (test_png.hpp): redistributable, no third-party asset.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "hcs/engines/llama_engine.hpp"
#include "hcs/job_queue.hpp"
#include "temp_dir.hpp"
#include "test_png.hpp"

using namespace hcs;
using namespace hcs::engines;
using namespace std::chrono_literals;

namespace {

std::string env(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

#define REQUIRE_VLM_OR_SKIP()                                                                         \
    const std::string vlm = env("HCS_TEST_VLM"), mmproj = env("HCS_TEST_MMPROJ");                     \
    if (vlm.empty() || mmproj.empty())                                                                \
    GTEST_SKIP() << "NOT RUN: HCS_TEST_VLM / HCS_TEST_MMPROJ not set (real multimodal-model test)"

struct Run {
    JobState state = JobState::Queued;
    std::string text;
    std::optional<Error> error;
};

LoadRequest vlm_load(const std::string& model, const std::string& proj, Backend b = Backend::Cpu) {
    LoadRequest lr;
    lr.model_path = model;
    lr.projector_path = proj;
    lr.backend = b;
    lr.context_tokens = 4096;
    return lr;
}

Run run_vision(JobQueue& q, LlamaTextEngine& e, VisionRequest req) {
    Run r;
    std::mutex m;
    auto id = e.analyze_image(req, [&](const JobEvent& ev) {
        if (ev.kind == JobEventKind::Output) {
            std::lock_guard l(m);
            r.text += ev.text;
        }
    });
    if (!id.ok()) {
        r.state = JobState::Failed;
        r.error = id.error();
        return r;
    }
    EXPECT_TRUE(q.wait(id.value(), 600s));
    auto st = q.status(id.value());
    r.state = st.value().state;
    r.error = st.value().error;
    return r;
}

VisionRequest red_square_request(
    const std::string& prompt = "What colour is the large square in this image? Answer in one short sentence.") {
    VisionRequest v;
    v.image.bytes = test::make_red_square_png(256);
    v.text.prompt = prompt;
    v.text.temperature = 0.0f;
    v.text.max_tokens = 40;
    return v;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c == '\n') {
            o += "\\n";
        } else if (c < 0x20) {
            o += ' ';
        } else {
            o += static_cast<char>(c);
        }
    }
    return o;
}

void evidence(const std::string& name, const std::string& json_fields) {
    const std::string dir = env("HCS_TEST_EVIDENCE_DIR");
    if (dir.empty()) return;
    std::ofstream f(dir + "/llama_vision.jsonl", std::ios::app);
    f << "{\"test\":\"" << name << "\"," << json_fields << "}\n";
}

}  // namespace

TEST(LlamaVisionReal, DescribesAGeneratedImageOnCpu) {
    REQUIRE_VLM_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    ASSERT_TRUE(e.load(vlm_load(vlm, mmproj)).ok());
    ASSERT_TRUE(e.vision_loaded());
    auto r = run_vision(q, e, red_square_request());
    ASSERT_EQ(r.state, JobState::Succeeded) << (r.error ? r.error->message : "");
    EXPECT_FALSE(r.text.empty());
    const auto m = e.metrics();
    // The prompt alone is ~30 tokens; image chunks add dozens more, so a large count proves the image was consumed.
    EXPECT_GT(m.prompt_tokens, 60u);
    EXPECT_GT(m.decode_tokens, 0u);
    std::string low = r.text;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::cout << "[vision answer] " << r.text << "\n";
    evidence("DescribesAGeneratedImageOnCpu",
             "\"backend\":\"" + m.backend + "\",\"prompt_tokens\":" + std::to_string(m.prompt_tokens) +
                 ",\"decode_tokens\":" + std::to_string(m.decode_tokens) + ",\"ttft_ms\":" + std::to_string(m.ttft_ms) +
                 ",\"decode_tps\":" + std::to_string(m.decode_tokens_per_second) + ",\"mentions_red\":" +
                 (low.find("red") != std::string::npos ? "true" : "false") + ",\"text\":\"" + json_escape(r.text) + "\"");
    EXPECT_NE(low.find("red"), std::string::npos) << "answer: " << r.text;
}

TEST(LlamaVisionReal, DescribesAGeneratedImageOnVulkan) {
    REQUIRE_VLM_OR_SKIP();
    if (LlamaTextEngine::gpu_devices().empty())
        GTEST_SKIP() << "NOT RUN: no GPU device visible to this build (vulkan_compiled=" +
                            std::to_string(LlamaTextEngine::vulkan_compiled()) + ")";
    JobQueue q;
    LlamaTextEngine e(q);
    ASSERT_TRUE(e.load(vlm_load(vlm, mmproj, Backend::Vulkan)).ok());
    ASSERT_TRUE(e.vision_loaded());
    auto r = run_vision(q, e, red_square_request());
    ASSERT_EQ(r.state, JobState::Succeeded) << (r.error ? r.error->message : "");
    const auto m = e.metrics();
    EXPECT_EQ(m.backend, "vulkan");
    EXPECT_GT(m.prompt_tokens, 60u);
    std::string low = r.text;
    for (auto& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::cout << "[vision answer, vulkan] " << r.text << " (device " << m.device << ")" << std::endl;
    evidence("DescribesAGeneratedImageOnVulkan",
             "\"backend\":\"" + m.backend + "\",\"device\":\"" + json_escape(m.device) + "\",\"prompt_tokens\":" +
                 std::to_string(m.prompt_tokens) + ",\"ttft_ms\":" + std::to_string(m.ttft_ms) + ",\"text\":\"" +
                 json_escape(r.text) + "\"");
    EXPECT_NE(low.find("red"), std::string::npos) << "answer: " << r.text;
}

TEST(LlamaVisionReal, GreedyOutputIsDeterministicAndEngineSurvivesRepeatsAndTextJobs) {
    REQUIRE_VLM_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    ASSERT_TRUE(e.load(vlm_load(vlm, mmproj)).ok());
    auto a = run_vision(q, e, red_square_request());
    auto b = run_vision(q, e, red_square_request());
    ASSERT_EQ(a.state, JobState::Succeeded);
    ASSERT_EQ(b.state, JobState::Succeeded);
    EXPECT_EQ(a.text, b.text);
    VisionRequest other = red_square_request("Describe the picture briefly.");
    other.image.bytes = test::make_png_rgb(64, 64, [](std::uint32_t x, std::uint32_t) {
        return std::array<std::uint8_t, 3>{0, 0, static_cast<std::uint8_t>(x * 4)};
    });
    EXPECT_EQ(run_vision(q, e, other).state, JobState::Succeeded);
    // text-only generation on the same vision session still works
    TextRequest t;
    t.prompt = "Say hello.";
    t.max_tokens = 8;
    t.temperature = 0.0f;
    std::string text;
    std::mutex m;
    auto id = e.generate(t, [&](const JobEvent& ev) {
        if (ev.kind == JobEventKind::Output) {
            std::lock_guard l(m);
            text += ev.text;
        }
    });
    ASSERT_TRUE(id.ok());
    ASSERT_TRUE(q.wait(id.value(), 300s));
    EXPECT_EQ(q.status(id.value()).value().state, JobState::Succeeded);
    EXPECT_FALSE(text.empty());
}

TEST(LlamaVisionReal, UnloadAndReloadReleasesTheProjector) {
    REQUIRE_VLM_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(e.load(vlm_load(vlm, mmproj)).ok()) << i;
        EXPECT_TRUE(e.vision_loaded());
        ASSERT_TRUE(e.unload().ok());
        EXPECT_FALSE(e.vision_loaded());
        EXPECT_FALSE(e.loaded());
    }
}

TEST(LlamaVisionReal, BadImagesFailCleanlyWithoutKillingTheEngine) {
    REQUIRE_VLM_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    ASSERT_TRUE(e.load(vlm_load(vlm, mmproj)).ok());

    VisionRequest garbage = red_square_request();
    garbage.image.bytes.assign(512, 0x42);  // not an image
    auto g = run_vision(q, e, garbage);
    EXPECT_EQ(g.state, JobState::Failed);
    ASSERT_TRUE(g.error.has_value());
    EXPECT_EQ(g.error->code, ErrorCode::InvalidArgument) << g.error->message;

    VisionRequest huge = red_square_request();
    huge.image.bytes = test::make_png_header_only(6000, 4000);  // 24 Mpixel declared, no pixel data
    auto h = run_vision(q, e, huge);
    EXPECT_EQ(h.state, JobState::Failed);
    ASSERT_TRUE(h.error.has_value());
    EXPECT_EQ(h.error->code, ErrorCode::InvalidArgument);
    EXPECT_NE(h.error->message.find("Mpixel"), std::string::npos) << h.error->message;

    // dimensions beyond what the decoder itself accepts (stb: > 2^30 bytes) are rejected as well
    VisionRequest absurd = red_square_request();
    absurd.image.bytes = test::make_png_header_only(60000, 60000);
    auto ab = run_vision(q, e, absurd);
    EXPECT_EQ(ab.state, JobState::Failed);
    ASSERT_TRUE(ab.error.has_value());
    EXPECT_EQ(ab.error->code, ErrorCode::InvalidArgument);

    VisionRequest truncated = red_square_request();
    truncated.image.bytes.resize(truncated.image.bytes.size() / 2);  // valid header, cut-off data
    auto t = run_vision(q, e, truncated);
    EXPECT_EQ(t.state, JobState::Failed);  // decode failure, not a crash

    VisionRequest marker = red_square_request("<__media__> sneaky");
    auto mk = e.analyze_image(marker);
    ASSERT_FALSE(mk.ok());
    EXPECT_EQ(mk.error().code, ErrorCode::InvalidArgument);

    // still healthy afterwards
    EXPECT_EQ(run_vision(q, e, red_square_request()).state, JobState::Succeeded);
}

TEST(LlamaVisionReal, CancellationStopsAVisionJob) {
    REQUIRE_VLM_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    ASSERT_TRUE(e.load(vlm_load(vlm, mmproj)).ok());
    VisionRequest v = red_square_request("Write a very long story about this image.");
    v.text.max_tokens = 400;
    std::atomic<int> pieces{0};
    auto id = e.analyze_image(v, [&](const JobEvent& ev) {
        if (ev.kind == JobEventKind::Output) ++pieces;
    });
    ASSERT_TRUE(id.ok());
    for (int i = 0; i < 6000 && pieces.load() < 3; ++i) std::this_thread::sleep_for(10ms);
    ASSERT_TRUE(q.cancel(id.value()).ok());
    ASSERT_TRUE(q.wait(id.value(), 120s));
    EXPECT_EQ(q.status(id.value()).value().state, JobState::Cancelled);
    EXPECT_LT(pieces.load(), 390);
    EXPECT_EQ(run_vision(q, e, red_square_request()).state, JobState::Succeeded);  // context reusable
}

TEST(LlamaVisionReal, TextOnlyLoadReportsMissingProjectorCleanly) {
    REQUIRE_VLM_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    LoadRequest lr = vlm_load(vlm, "");
    ASSERT_TRUE(e.load(lr).ok());
    EXPECT_FALSE(e.vision_loaded());
    auto r = e.analyze_image(red_square_request());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_NE(r.error().message.find("projector"), std::string::npos);
}

TEST(LlamaVisionReal, MissingOrBrokenProjectorFilesAreCleanLoadErrors) {
    REQUIRE_VLM_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    auto miss = e.load(vlm_load(vlm, "Z:/definitely/not/here/mmproj.gguf"));
    ASSERT_FALSE(miss.ok());
    EXPECT_EQ(miss.error().code, ErrorCode::NotFound);
    test::TempDir d;
    test::write_file(d / "mmproj-fake.gguf", "not a gguf at all");
    auto bad_magic = e.load(vlm_load(vlm, (d / "mmproj-fake.gguf").string()));
    ASSERT_FALSE(bad_magic.ok());
    EXPECT_EQ(bad_magic.error().code, ErrorCode::ModelError);
    test::write_file(d / "mmproj-trunc.gguf", test::fake_gguf());  // GGUF magic, no content
    auto trunc = e.load(vlm_load(vlm, (d / "mmproj-trunc.gguf").string()));
    ASSERT_FALSE(trunc.ok());
    EXPECT_FALSE(e.loaded());                        // failed load leaves nothing behind
    EXPECT_TRUE(e.load(vlm_load(vlm, mmproj)).ok());  // and the engine is reusable
}

TEST(LlamaVisionReal, ProjectorThatDoesNotMatchTheTextModelIsRejected) {
    REQUIRE_VLM_OR_SKIP();
    const std::string text_model = env("HCS_TEST_GGUF");
    if (text_model.empty()) GTEST_SKIP() << "NOT RUN: HCS_TEST_GGUF (a plain text GGUF) is not set";
    JobQueue q;
    LlamaTextEngine e(q);
    auto r = e.load(vlm_load(text_model, mmproj));  // SmolVLM projector on a Qwen3 text model
    ASSERT_FALSE(r.ok()) << "a mismatched projector must not load";
    std::cout << "[mismatch] " << to_string(r.error().code) << ": " << r.error().message << "\n";
    EXPECT_FALSE(e.loaded());
    evidence("ProjectorThatDoesNotMatchTheTextModelIsRejected",
             "\"code\":\"" + std::string(to_string(r.error().code)) + "\",\"message\":\"" +
                 json_escape(r.error().message) + "\"");
}

TEST(LlamaVisionReal, LoadingAProjectorAsTheMainModelIsAnUnsupportedError) {
    REQUIRE_VLM_OR_SKIP();
    JobQueue q;
    LlamaTextEngine e(q);
    LoadRequest lr;
    lr.model_path = mmproj;  // clip-architecture file passed as the language model
    lr.backend = Backend::Cpu;
    auto r = e.load(lr);
    ASSERT_FALSE(r.ok());
    std::cout << "[mmproj-as-model] " << to_string(r.error().code) << ": " << r.error().message << "\n";
    EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
    EXPECT_FALSE(e.loaded());
}
