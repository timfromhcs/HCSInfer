#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "hcs/result.hpp"
#include "hcs/train/adapter_book.hpp"
#include "hcs/train/eval_report.hpp"
#include "hcs/train/pipeline.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::train;
using hcs::test::TempDir;
using nlohmann::json;

namespace {

std::string hex(char c) {
    return std::string(64, c);
}

EvalReport report(const std::string& eval_id, char set, double heldout, std::size_t samples = 100) {
    json j{
        {"schema_version", 1},
        {"eval_id", eval_id},
        {"set_sha256", hex(set)},
        {"heldout", {{"samples", samples}, {"score", heldout}}},
        {"regression", json::array({json{{"name", "coding"}, {"samples", samples / 2}, {"score", 0.9}},
                                    json{{"name", "tool_use"}, {"samples", samples / 2}, {"score", 0.9}}})}};
    auto parsed = parse_eval_report(j.dump());
    return parsed.value();
}

PipelineRequest request(const std::string& id, double base_heldout, double cand_heldout) {
    PipelineRequest q;
    q.adapter_id = id;
    q.base_model_sha256 = hex('a');
    q.adapter_sha256 = hex('b');
    q.artifact_path = "D:/models/adapters/" + id + ".gguf";
    q.dataset_manifest_sha256 = hex('c');
    q.hyperparameters = {{"rank", "8"}};
    q.baseline = report("agentic-test", 'd', base_heldout);
    q.candidate = report("agentic-test", 'd', cand_heldout);
    q.created_at = "2026-11-17T00:00:00Z";
    return q;
}

}  // namespace

TEST(TrainPipeline, KeepInstallsAndActivates) {
    TempDir dir;
    AdapterBook book(dir / "train");
    auto out = run_train_pipeline(request("keep-1", 0.50, 0.60), book);
    ASSERT_TRUE(out) << out.error().message;
    EXPECT_EQ(out.value().gate.decision, GateDecision::Keep);
    EXPECT_TRUE(out.value().installed);
    EXPECT_TRUE(out.value().activated);
    ASSERT_EQ(book.active(hex('a')).value()->adapter_id, "keep-1");
}

TEST(TrainPipeline, RollbackIsRecordedButNotActivated) {
    TempDir dir;
    AdapterBook book(dir / "train");
    auto out = run_train_pipeline(request("roll-1", 0.60, 0.55), book);
    ASSERT_TRUE(out) << out.error().message;
    EXPECT_EQ(out.value().gate.decision, GateDecision::Rollback);
    EXPECT_TRUE(out.value().installed);
    EXPECT_FALSE(out.value().activated);

    // Audit trail exists, but nothing is active.
    auto manifest = book.get("roll-1");
    ASSERT_TRUE(manifest) << manifest.error().message;
    EXPECT_EQ(manifest.value().gate.decision, GateDecision::Rollback);
    EXPECT_NEAR(manifest.value().candidate.heldout, 0.55, 1e-9);
    EXPECT_FALSE(book.active(hex('a')).value().has_value());
}

TEST(TrainPipeline, InvalidGateIsRecordedNotActivated) {
    TempDir dir;
    AdapterBook book(dir / "train");
    PipelineRequest q = request("bad-sizes", 0.50, 0.60);
    q.candidate = report("agentic-test", 'd', 0.60, 50);  // different sample count -> incomparable evidence
    auto out = run_train_pipeline(q, book);
    ASSERT_TRUE(out) << out.error().message;
    EXPECT_EQ(out.value().gate.decision, GateDecision::Invalid);
    EXPECT_TRUE(out.value().installed);
    EXPECT_FALSE(out.value().activated);
}

TEST(TrainPipeline, IncomparableReportsAreRejectedBeforeInstall) {
    TempDir dir;
    AdapterBook book(dir / "train");
    PipelineRequest q = request("mismatch", 0.50, 0.60);
    q.candidate = report("other-eval", 'd', 0.60);
    auto out = run_train_pipeline(q, book);
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code, ErrorCode::InvalidArgument);
    // Nothing was written.
    EXPECT_TRUE(book.list().value().empty());
}

TEST(TrainPipeline, RejectsBadHashesAndIds) {
    TempDir dir;
    AdapterBook book(dir / "train");
    {
        PipelineRequest q = request("x", 0.5, 0.6);
        q.adapter_sha256 = "short";
        auto out = run_train_pipeline(q, book);
        ASSERT_FALSE(out);
        EXPECT_EQ(out.error().code, ErrorCode::InvalidArgument);
    }
    {
        PipelineRequest q = request("../escape", 0.5, 0.6);
        auto out = run_train_pipeline(q, book);
        ASSERT_FALSE(out);
        EXPECT_EQ(out.error().code, ErrorCode::InvalidArgument);
    }
    {
        PipelineRequest q = request("x", 0.5, 0.6);
        q.dataset_manifest_sha256 = "";
        auto out = run_train_pipeline(q, book);
        ASSERT_FALSE(out);
    }
}

TEST(TrainPipeline, DuplicateAdapterIdIsRejected) {
    TempDir dir;
    AdapterBook book(dir / "train");
    ASSERT_TRUE(run_train_pipeline(request("once", 0.5, 0.6), book));
    auto second = run_train_pipeline(request("once", 0.5, 0.6), book);
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, ErrorCode::AlreadyExists);
}
