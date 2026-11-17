#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/train/adapter_book.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::train;
using hcs::test::TempDir;

namespace fs = std::filesystem;

namespace {

std::string hex(char c) {
    return std::string(64, c);
}

EvalScores scores(double heldout) {
    EvalScores s;
    s.heldout = heldout;
    s.heldout_samples = 100;
    s.regression = {{"coding", 0.9}, {"tool_use", 0.85}};
    return s;
}

AdapterManifest make_manifest(const std::string& id, char base = 'a',
                              GateDecision decision = GateDecision::Keep) {
    AdapterManifest m;
    m.adapter_id = id;
    m.base_model_sha256 = hex(base);
    m.adapter_sha256 = hex('b');
    m.artifact_path = "D:/models/adapters/" + id + ".gguf";
    m.dataset_manifest_sha256 = hex('c');
    m.eval_id = "agentic-test";
    m.hyperparameters = {{"rank", "8"}};
    m.baseline = scores(0.50);
    m.candidate = scores(decision == GateDecision::Keep ? 0.60 : 0.45);
    m.gate = GateResult{decision, "unit test", decision == GateDecision::Keep ? 0.10 : -0.05, {}};
    m.created_at = "2026-11-17T00:00:00Z";
    return m;
}

std::vector<std::string> ids(const std::vector<AdapterManifest>& v) {
    std::vector<std::string> out;
    for (const auto& m : v) out.push_back(m.adapter_id);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST(AdapterBook, InstallGetAndList) {
    TempDir dir;
    AdapterBook book(dir / "train");
    ASSERT_TRUE(book.install(make_manifest("a-0001")));
    ASSERT_TRUE(book.install(make_manifest("a-0002")));

    auto got = book.get("a-0001");
    ASSERT_TRUE(got) << got.error().message;
    EXPECT_EQ(got.value().eval_id, "agentic-test");
    EXPECT_EQ(got.value().hyperparameters.at("rank"), "8");

    auto all = book.list();
    ASSERT_TRUE(all);
    EXPECT_EQ(ids(all.value()), (std::vector<std::string>{"a-0001", "a-0002"}));
}

TEST(AdapterBook, DuplicateIdFailsUnlessOverwrite) {
    TempDir dir;
    AdapterBook book(dir / "train");
    ASSERT_TRUE(book.install(make_manifest("dup")));
    auto again = book.install(make_manifest("dup"));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::AlreadyExists);
    EXPECT_TRUE(book.install(make_manifest("dup"), /*overwrite=*/true));
}

TEST(AdapterBook, MissingAdapterIsNotFound) {
    TempDir dir;
    AdapterBook book(dir / "train");
    auto got = book.get("does-not-exist");
    ASSERT_FALSE(got);
    EXPECT_EQ(got.error().code, ErrorCode::NotFound);
}

TEST(AdapterBook, ActivateIsRefusedUnlessTheGateKeptIt) {
    TempDir dir;
    AdapterBook book(dir / "train");
    ASSERT_TRUE(book.install(make_manifest("rolled", 'a', GateDecision::Rollback)));
    ASSERT_TRUE(book.install(make_manifest("invalid", 'a', GateDecision::Invalid)));

    auto r1 = book.activate("rolled");
    ASSERT_FALSE(r1);
    EXPECT_EQ(r1.error().code, ErrorCode::InvalidState);
    auto r2 = book.activate("invalid");
    ASSERT_FALSE(r2);
    EXPECT_EQ(r2.error().code, ErrorCode::InvalidState);
    // Nothing was promoted.
    auto active = book.active(hex('a'));
    ASSERT_TRUE(active);
    EXPECT_FALSE(active.value().has_value());
}

TEST(AdapterBook, ActivateThenActiveReportsIt) {
    TempDir dir;
    AdapterBook book(dir / "train");
    ASSERT_TRUE(book.install(make_manifest("keep-1")));
    ASSERT_TRUE(book.activate("keep-1"));
    auto active = book.active(hex('a'));
    ASSERT_TRUE(active) << active.error().message;
    ASSERT_TRUE(active.value().has_value());
    EXPECT_EQ(active.value()->adapter_id, "keep-1");
    // activating again is idempotent
    EXPECT_TRUE(book.activate("keep-1"));
}

TEST(AdapterBook, RollbackMovesThePointerBackWithoutDeleting) {
    TempDir dir;
    AdapterBook book(dir / "train");
    ASSERT_TRUE(book.install(make_manifest("v1")));
    ASSERT_TRUE(book.install(make_manifest("v2")));
    ASSERT_TRUE(book.activate("v1"));
    ASSERT_TRUE(book.activate("v2"));
    ASSERT_EQ(book.active(hex('a')).value()->adapter_id, "v2");

    ASSERT_TRUE(book.rollback(hex('a')));
    EXPECT_EQ(book.active(hex('a')).value()->adapter_id, "v1");

    // The rolled-back adapter still exists.
    EXPECT_TRUE(book.get("v2"));

    // A second rollback swaps again (v1 was the previous).
    ASSERT_TRUE(book.rollback(hex('a')));
    EXPECT_EQ(book.active(hex('a')).value()->adapter_id, "v2");
}

TEST(AdapterBook, RollbackWithoutActivationIsNotFound) {
    TempDir dir;
    AdapterBook book(dir / "train");
    ASSERT_TRUE(book.install(make_manifest("only")));
    auto r = book.rollback(hex('a'));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    auto unknown = book.rollback(hex('z'));
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::NotFound);
}

TEST(AdapterBook, RejectsUnsafeAdapterIds) {
    TempDir dir;
    AdapterBook book(dir / "train");
    for (const char* bad : {"../escape", "a/b", "a\\b", "CON", "con", "nul", ".hidden", "trailing.", ""}) {
        auto m = make_manifest("ok");
        m.adapter_id = bad;
        auto r = book.install(m);
        EXPECT_FALSE(r) << "id '" << bad << "' should be rejected";
    }
    EXPECT_FALSE(book.get("../escape"));
}

TEST(AdapterBook, ActiveOnAnUnknownBaseIsEmptyNotAnError) {
    TempDir dir;
    AdapterBook book(dir / "train");
    auto r = book.active(hex('z'));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_FALSE(r.value().has_value());
}

TEST(AdapterBook, CorruptManifestFailsClosed) {
    TempDir dir;
    AdapterBook book(dir / "train");
    ASSERT_TRUE(book.install(make_manifest("good")));
    hcs::test::write_file(dir / "train" / "adapters" / "broken" / "manifest.json", "{ this is not json");
    EXPECT_FALSE(book.get("broken"));
    EXPECT_FALSE(book.list());
}

TEST(AdapterBook, PointerPersistsAcrossInstances) {
    TempDir dir;
    {
        AdapterBook book(dir / "train");
        ASSERT_TRUE(book.install(make_manifest("persist")));
        ASSERT_TRUE(book.activate("persist"));
    }
    AdapterBook reopened(dir / "train");
    auto active = reopened.active(hex('a'));
    ASSERT_TRUE(active) << active.error().message;
    ASSERT_TRUE(active.value().has_value());
    EXPECT_EQ(active.value()->adapter_id, "persist");
}

TEST(AdapterBook, ManifestJsonRoundTripsAndRejectsDefects) {
    const AdapterManifest m = make_manifest("rt-1");
    auto parsed = parse_adapter_manifest(to_json(m));
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().adapter_id, "rt-1");
    EXPECT_EQ(parsed.value().gate.decision, GateDecision::Keep);
    EXPECT_NEAR(parsed.value().candidate.heldout, m.candidate.heldout, 1e-9);
    EXPECT_NEAR(parsed.value().baseline.regression.at("coding"), 0.9, 1e-9);

    EXPECT_FALSE(parse_adapter_manifest("{ not json"));
    EXPECT_FALSE(parse_adapter_manifest(to_json(m).substr(0, 20)));
    EXPECT_FALSE(parse_adapter_manifest("{\"schema_version\":1}"));
}
