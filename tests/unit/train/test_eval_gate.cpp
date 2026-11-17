#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "hcs/train/eval_gate.hpp"

using namespace hcs::train;

namespace {

EvalScores base() {
    EvalScores s;
    s.heldout = 0.50;
    s.heldout_samples = 100;
    s.regression = {{"tool_format", 0.90}, {"code_basic", 0.70}, {"chat_sanity", 0.80}};
    return s;
}

EvalScores better() {
    EvalScores s = base();
    s.heldout = 0.58;
    return s;
}

}  // namespace

TEST(EvalGate, KeepsWhenHeldoutImprovesAndNothingRegresses) {
    const auto r = evaluate_gate(base(), better());
    EXPECT_EQ(r.decision, GateDecision::Keep) << r.reason;
    EXPECT_NEAR(r.heldout_delta, 0.08, 1e-9);
    EXPECT_TRUE(r.regressed_suites.empty());
    EXPECT_FALSE(r.reason.empty());
}

TEST(EvalGate, KeepsWhenRegressionSuitesImproveOrStayEqual) {
    EvalScores c = better();
    c.regression["tool_format"] = 0.95;  // up
    EXPECT_EQ(evaluate_gate(base(), c).decision, GateDecision::Keep);
}

TEST(EvalGate, RollsBackWhenAnyRegressionSuiteDrops) {
    EvalScores c = better();
    c.regression["code_basic"] = 0.69;
    const auto r = evaluate_gate(base(), c);
    EXPECT_EQ(r.decision, GateDecision::Rollback);
    ASSERT_EQ(r.regressed_suites.size(), 1u);
    EXPECT_EQ(r.regressed_suites[0], "code_basic");
}

TEST(EvalGate, ToleranceAllowsSmallNoiseOnlyWhenConfigured) {
    EvalScores c = better();
    c.regression["code_basic"] = 0.69;
    GatePolicy tolerant;
    tolerant.max_suite_drop = 0.02;
    EXPECT_EQ(evaluate_gate(base(), c, tolerant).decision, GateDecision::Keep);
    c.regression["code_basic"] = 0.67;  // 0.03 drop > 0.02
    EXPECT_EQ(evaluate_gate(base(), c, tolerant).decision, GateDecision::Rollback);
}

TEST(EvalGate, RollsBackWhenHeldoutDoesNotImproveEnough) {
    EvalScores same = base();
    EXPECT_EQ(evaluate_gate(base(), same).decision, GateDecision::Rollback);
    EvalScores worse = base();
    worse.heldout = 0.45;
    EXPECT_EQ(evaluate_gate(base(), worse).decision, GateDecision::Rollback);
    EvalScores tiny = base();
    tiny.heldout = 0.505;  // +0.005 < default 0.01
    EXPECT_EQ(evaluate_gate(base(), tiny).decision, GateDecision::Rollback);
    EvalScores exact = base();
    exact.heldout = 0.51;  // exactly the minimum gain (float noise must not flip this)
    EXPECT_EQ(evaluate_gate(base(), exact).decision, GateDecision::Keep);
}

TEST(EvalGate, GainWithRegressionIsStillRollback) {
    EvalScores c = better();
    c.heldout = 0.99;
    c.regression["chat_sanity"] = 0.10;
    EXPECT_EQ(evaluate_gate(base(), c).decision, GateDecision::Rollback);
}

TEST(EvalGate, IncomparableInputsAreInvalidAndFailClosed) {
    {
        EvalScores c = better();
        c.regression.erase("tool_format");  // candidate silently omits a suite
        const auto r = evaluate_gate(base(), c);
        EXPECT_EQ(r.decision, GateDecision::Invalid);
        EXPECT_NE(r.reason.find("tool_format"), std::string::npos);
    }
    {
        EvalScores c = better();
        c.regression["extra"] = 1.0;
        EXPECT_EQ(evaluate_gate(base(), c).decision, GateDecision::Invalid);
    }
    {
        EvalScores c = better();
        c.heldout_samples = 99;
        EXPECT_EQ(evaluate_gate(base(), c).decision, GateDecision::Invalid);
    }
    {
        EvalScores b = base();
        EvalScores c = better();
        b.heldout_samples = c.heldout_samples = 10;  // below min_heldout_samples (50)
        EXPECT_EQ(evaluate_gate(b, c).decision, GateDecision::Invalid);
    }
    {
        EvalScores b = base();
        b.regression.clear();
        EvalScores c = better();
        c.regression.clear();
        EXPECT_EQ(evaluate_gate(b, c).decision,
                  GateDecision::Invalid);  // no regression set: cannot prove safety
    }
}

TEST(EvalGate, NonFiniteOrOutOfRangeScoresAreInvalid) {
    for (const double bad :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -0.1, 1.1}) {
        EvalScores c = better();
        c.heldout = bad;
        EXPECT_EQ(evaluate_gate(base(), c).decision, GateDecision::Invalid) << bad;
        EvalScores d = better();
        d.regression["code_basic"] = bad;
        EXPECT_EQ(evaluate_gate(base(), d).decision, GateDecision::Invalid) << bad;
        EvalScores b = base();
        b.heldout = bad;
        EXPECT_EQ(evaluate_gate(b, better()).decision, GateDecision::Invalid) << bad;
    }
    GatePolicy p;
    p.min_heldout_gain = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(evaluate_gate(base(), better(), p).decision, GateDecision::Invalid);
    EXPECT_STREQ(to_string(GateDecision::Keep), "keep");
    EXPECT_STREQ(to_string(GateDecision::Rollback), "rollback");
    EXPECT_STREQ(to_string(GateDecision::Invalid), "invalid");
}
