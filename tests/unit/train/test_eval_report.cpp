#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "hcs/result.hpp"
#include "hcs/train/eval_report.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::train;
using hcs::test::TempDir;
using nlohmann::json;

namespace {

std::string hex(char c) {
    return std::string(64, c);
}

json valid_report() {
    return json{{"schema_version", 1},
                {"eval_id", "agentic-lfm3"},
                {"set_sha256", hex('a')},
                {"heldout", {{"samples", 40}, {"score", 0.925}}},
                {"regression", json::array({json{{"name", "coding"}, {"samples", 8}, {"score", 1.0}},
                                            json{{"name", "tool_use"}, {"samples", 20}, {"score", 0.9}}})}};
}

}  // namespace

TEST(EvalReport, ParsesAValidReport) {
    auto r = parse_eval_report(valid_report().dump());
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().eval_id, "agentic-lfm3");
    EXPECT_EQ(r.value().heldout_samples, 40u);
    EXPECT_NEAR(r.value().heldout_score, 0.925, 1e-9);
    EXPECT_EQ(r.value().regression.size(), 2u);
    EXPECT_NEAR(r.value().regression.at("coding").score, 1.0, 1e-9);
    EXPECT_EQ(r.value().regression.at("tool_use").samples, 20u);
    EXPECT_EQ(r.value().set_sha256, hex('a'));
}

TEST(EvalReport, RoundTripsThroughJson) {
    auto first = parse_eval_report(valid_report().dump());
    ASSERT_TRUE(first);
    auto second = parse_eval_report(to_json(first.value()));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second.value().eval_id, first.value().eval_id);
    EXPECT_EQ(second.value().heldout_samples, first.value().heldout_samples);
    EXPECT_EQ(second.value().regression.size(), first.value().regression.size());
}

TEST(EvalReport, ScoresFeedTheGate) {
    auto r = parse_eval_report(valid_report().dump());
    ASSERT_TRUE(r);
    const EvalScores s = r.value().scores();
    EXPECT_NEAR(s.heldout, 0.925, 1e-9);
    EXPECT_EQ(s.heldout_samples, 40u);
    EXPECT_NEAR(s.regression.at("coding"), 1.0, 1e-9);
}

TEST(EvalReport, RejectsMissingOrMalformedFields) {
    const auto expect_invalid = [](json j) {
        auto r = parse_eval_report(j.dump());
        EXPECT_FALSE(r) << "expected a rejection for " << j.dump();
        if (!r) {
            EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
            EXPECT_FALSE(r.error().message.empty());
        }
    };

    expect_invalid(json::object());  // no schema_version
    json wrong_version = valid_report();
    wrong_version["schema_version"] = 2;
    expect_invalid(wrong_version);
    json bad_eval = valid_report();
    bad_eval["eval_id"] = "../escape";
    expect_invalid(bad_eval);
    json bad_hash = valid_report();
    bad_hash["set_sha256"] = "not-a-hash";
    expect_invalid(bad_hash);
    json bad_score = valid_report();
    bad_score["heldout"]["score"] = 1.5;
    expect_invalid(bad_score);
    json zero_samples = valid_report();
    zero_samples["heldout"]["samples"] = 0;
    expect_invalid(zero_samples);
    json no_regression = valid_report();
    no_regression["regression"] = json::array();
    expect_invalid(no_regression);
    json dup_suite = valid_report();
    dup_suite["regression"].push_back(json{{"name", "coding"}, {"samples", 3}, {"score", 0.5}});
    expect_invalid(dup_suite);
    json bad_suite_score = valid_report();
    bad_suite_score["regression"][0]["score"] = -0.1;
    expect_invalid(bad_suite_score);
}

TEST(EvalReport, RejectsNonJsonAndDeepNesting) {
    EXPECT_FALSE(parse_eval_report("not json"));
    EXPECT_FALSE(parse_eval_report(""));
    const std::string deep = std::string(40, '[') + std::string(40, ']');
    auto r = parse_eval_report(deep);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("deep"), std::string::npos);
}

TEST(EvalReport, ReadsFromAFile) {
    TempDir dir;
    const auto path = dir / "baseline.json";
    hcs::test::write_file(path, valid_report().dump());
    auto r = parse_eval_report_file(path);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().eval_id, "agentic-lfm3");
    auto missing = parse_eval_report_file(dir / "nope.json");
    EXPECT_FALSE(missing);
}

TEST(EvalReport, ComparabilityRequiresSameSuiteAndSet) {
    auto a = parse_eval_report(valid_report().dump());
    auto b = parse_eval_report(valid_report().dump());
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    std::string reason;
    EXPECT_TRUE(comparable(a.value(), b.value(), reason));

    json other_id = valid_report();
    other_id["eval_id"] = "different";
    auto c = parse_eval_report(other_id.dump());
    ASSERT_TRUE(c);
    EXPECT_FALSE(comparable(a.value(), c.value(), reason));
    EXPECT_FALSE(reason.empty());

    json other_set = valid_report();
    other_set["set_sha256"] = hex('b');
    auto d = parse_eval_report(other_set.dump());
    ASSERT_TRUE(d);
    reason.clear();
    EXPECT_FALSE(comparable(a.value(), d.value(), reason));
    EXPECT_NE(reason.find("task sets"), std::string::npos);
}
