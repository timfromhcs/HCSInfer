// RSI loop tests. The loop reuses main's EvalReport / eval gate / run_train_pipeline / AdapterBook; the
// trainer is a scripted hook. A real trainer run is NOT RUN here (there is no trainer in CI): these tests
// prove the loop's control flow, bounds and gate discipline, not model quality.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "hcs/train/adapter_book.hpp"
#include "hcs/train/rsi_loop.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::train;
using hcs::test::TempDir;
using nlohmann::json;

namespace {

std::string hex(char c) {
    return std::string(64, c);
}

Evaluation ev(double heldout, const std::vector<std::string>& failing = {"parallel"}, char set = 'd',
              std::size_t samples = 100, double regression = 0.9) {
    json j{{"schema_version", 1},
           {"eval_id", "agentic-test"},
           {"set_sha256", hex(set)},
           {"heldout", {{"samples", samples}, {"score", heldout}}},
           {"regression", json::array({json{{"name", "coding"}, {"samples", 50}, {"score", regression}},
                                       json{{"name", "tool_use"}, {"samples", 50}, {"score", 0.9}}})}};
    Evaluation e;
    e.report = parse_eval_report(j.dump()).value();
    for (std::size_t i = 0; i < failing.size(); ++i)
        e.failures.push_back({"f" + std::to_string(i), failing[i], "wrong"});
    return e;
}

struct Fixture {
    TempDir dir;
    AdapterBook book{dir / "train"};
    std::string base = hex('a');
    int trained = 0;
    int evaluations = 0;
    std::vector<std::string> evaluated_ids;
    std::map<std::string, double> score = {{"", 0.60}};  // held-out score by adapter id ("" = base model)
    std::map<std::string, double> regression;            // coding-suite score override by adapter id
    std::map<std::string, char> set;                     // task-set hash override by adapter id

    RsiHooks hooks() {
        RsiHooks h;
        h.evaluate = [this](const std::string& id) -> Result<Evaluation> {
            ++evaluations;
            evaluated_ids.push_back(id);
            const auto s = score.find(id);
            if (s == score.end())
                return Error(ErrorCode::NotFound, "no scripted score for '" + id + "'");
            const auto r = regression.find(id);
            const auto t = set.find(id);
            return ev(s->second, {"parallel"}, t != set.end() ? t->second : 'd', 100,
                      r != regression.end() ? r->second : 0.9);
        };
        h.train_candidate = [this](const std::vector<Proposal>&, int cycle) -> Result<Candidate> {
            ++trained;
            Candidate c;
            c.adapter_id = "c" + std::to_string(cycle);
            c.adapter_sha256 = hex('b');
            c.artifact_path = "D:/models/adapters/" + c.adapter_id + ".gguf";
            c.dataset_manifest_sha256 = hex('c');
            c.hyperparameters = {{"rank", "8"}};
            c.created_at = "2026-11-18T00:00:00Z";
            return c;
        };
        return h;
    }

    RsiPolicy policy(int cycles, int patience = 2) {
        RsiPolicy p;
        p.max_cycles = cycles;
        p.patience = patience;
        return p;
    }

    std::string active_id() {
        auto a = book.active(base);
        EXPECT_TRUE(a) << (a ? "" : a.error().message);
        return a && a.value() ? a.value()->adapter_id : std::string();
    }
    std::size_t installed() {
        auto l = book.list();
        return l ? l.value().size() : 9999;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// parse_eval_failures / reflect
// ---------------------------------------------------------------------------------------------------------------
TEST(EvalFailures, ReadsOnlyFailedEntriesAndTolerantOfAMissingResultsArray) {
    const auto f = parse_eval_failures(
        R"({"results":[{"id":"a","category":"parallel","pass":false,"reason":"wrong arg"},)"
        R"({"id":"b","category":"single","pass":true},{"id":"c","pass":false}]})");
    ASSERT_TRUE(f) << f.error().message;
    ASSERT_EQ(f.value().size(), 2u);
    EXPECT_EQ(f.value()[0].category, "parallel");
    EXPECT_EQ(f.value()[0].reason, "wrong arg");
    EXPECT_EQ(f.value()[1].category, "");
    const auto none = parse_eval_failures(R"({"heldout":{"samples":1,"score":1}})");
    ASSERT_TRUE(none);
    EXPECT_TRUE(none.value().empty());
}

TEST(EvalFailures, RejectsMalformedInputAndCleansHostileStrings) {
    for (const char* bad : {"", "not json", "[]", R"({"results":"x"})", R"({"results":[1]})",
                            R"({"results":[{"id":"a"}]})", R"({"results":[{"id":"a","pass":"no"}]})"})
        EXPECT_FALSE(parse_eval_failures(bad)) << bad;
    EXPECT_FALSE(parse_eval_failures(std::string(100000, '[') + std::string(100000, ']')));

    json big = {
        {"results", json::array({{{"id", std::string(1000, 'x')},
                                  {"category", "a\nb\tc\x01"},
                                  {"reason", std::string(60, 'a') + "\xE2\x82\xAC" + std::string(100, 'b')},
                                  {"pass", false}}})}};
    const auto f = parse_eval_failures(big.dump());
    ASSERT_TRUE(f);
    ASSERT_EQ(f.value().size(), 1u);
    EXPECT_LE(f.value()[0].id.size(), 128u);
    EXPECT_EQ(f.value()[0].category, "a b c ");
    EXPECT_LE(f.value()[0].reason.size(), 128u);
    // never ends inside a multi-byte sequence
    const std::string& r = f.value()[0].reason;
    EXPECT_TRUE(r.empty() || (static_cast<unsigned char>(r.back()) & 0xC0U) != 0xC0U);
}

TEST(Reflect, ProposesBoundedDataLargestFailingCategoryFirstTiesByName) {
    RsiPolicy p;
    p.examples_per_failure = 40;
    p.max_examples_per_category = 100;
    const auto props = reflect(ev(0.8, {"typed", "parallel", "parallel", "parallel", "alpha"}).failures, p);
    ASSERT_EQ(props.size(), 3u);
    EXPECT_EQ(props[0].category, "parallel");
    EXPECT_EQ(props[0].count, 100u);        // 3 * 40 capped at 100
    EXPECT_EQ(props[1].category, "alpha");  // equal counts: alphabetical, deterministic
    EXPECT_EQ(props[2].category, "typed");
    EXPECT_EQ(props[1].count, 40u);
    for (const auto& pr : props) EXPECT_EQ(pr.kind, "add_training_data");
}

TEST(Reflect, NothingFailingYieldsAStopProposalAndUncategorisedIsNamed) {
    const auto stop = reflect({}, RsiPolicy{});
    ASSERT_EQ(stop.size(), 1u);
    EXPECT_EQ(stop[0].kind, "stop");
    const auto un = reflect({EvalFailure{"x", "", "r"}}, RsiPolicy{});
    ASSERT_EQ(un.size(), 1u);
    EXPECT_EQ(un[0].category, "uncategorised");
}

TEST(Reflect, SizingCannotOverflow) {
    RsiPolicy p;
    p.examples_per_failure = std::numeric_limits<std::size_t>::max();
    p.max_examples_per_category = 500;
    const auto props = reflect(ev(0.5, {"a", "a", "a"}).failures, p);
    ASSERT_EQ(props.size(), 1u);
    EXPECT_EQ(props[0].count, 500u);
}

// ---------------------------------------------------------------------------------------------------------------
// run_rsi: gate discipline
// ---------------------------------------------------------------------------------------------------------------
TEST(RsiLoop, KeepsAnImprovingAdapterInstallsItAndActivatesIt) {
    Fixture f;
    f.score["c1"] = 0.75;
    const auto out = run_rsi(f.policy(1), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out) << out.error().message;
    ASSERT_EQ(out.value().cycles.size(), 1u);
    const CycleRecord& c = out.value().cycles[0];
    EXPECT_EQ(c.decision, GateDecision::Keep);
    EXPECT_TRUE(c.installed);
    EXPECT_TRUE(c.activated);
    EXPECT_NEAR(c.baseline_heldout, 0.60, 1e-9);
    EXPECT_NEAR(c.candidate_heldout, 0.75, 1e-9);
    EXPECT_EQ(out.value().final_adapter, "c1");
    EXPECT_EQ(f.active_id(), "c1");
    const auto m = f.book.get("c1");
    ASSERT_TRUE(m);
    EXPECT_EQ(m.value().gate.decision, GateDecision::Keep);
    EXPECT_EQ(m.value().base_model_sha256, f.base);
    EXPECT_EQ(m.value().hyperparameters.at("rank"), "8");
}

TEST(RsiLoop, ANonImprovingAdapterIsRecordedButNeverActivatedAndPatienceStopsTheLoop) {
    Fixture f;
    f.score["c1"] = 0.60;
    f.score["c2"] = 0.55;
    const auto out = run_rsi(f.policy(5, 2), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out);
    ASSERT_EQ(out.value().cycles.size(), 2u);
    for (const auto& c : out.value().cycles) {
        EXPECT_EQ(c.decision, GateDecision::Rollback);
        EXPECT_TRUE(c.installed) << "the audit trail is written even for a rejected adapter";
        EXPECT_FALSE(c.activated);
    }
    EXPECT_EQ(f.active_id(), "");
    EXPECT_EQ(out.value().final_adapter, "");
    EXPECT_EQ(f.book.get("c1").value().gate.decision, GateDecision::Rollback);
    EXPECT_EQ(f.installed(), 2u);
    EXPECT_NE(out.value().stop_reason.find("no accepted improvement"), std::string::npos);
}

TEST(RsiLoop, ImprovementThenStallStopsAtPatienceWithTheGoodAdapterActive) {
    Fixture f;
    f.score["c1"] = 0.80;
    f.score["c2"] = 0.80;  // no gain over c1: Rollback
    f.score["c3"] = 0.79;
    const auto out = run_rsi(f.policy(6, 2), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out);
    ASSERT_EQ(out.value().cycles.size(), 3u);
    EXPECT_EQ(out.value().final_adapter, "c1");
    EXPECT_EQ(f.active_id(), "c1");
    // cycle 2 was judged against the adapter that became active in cycle 1, not against the original base
    EXPECT_NEAR(out.value().cycles[1].baseline_heldout, 0.80, 1e-9);
}

TEST(RsiLoop, AGainThatRegressesASuiteIsNotKept) {
    Fixture f;
    f.score["c1"] = 0.90;
    f.regression["c1"] = 0.50;  // coding suite dropped from 0.90 to 0.50
    const auto out = run_rsi(f.policy(1), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out);
    EXPECT_EQ(out.value().cycles[0].decision, GateDecision::Rollback);
    EXPECT_FALSE(out.value().cycles[0].activated);
    EXPECT_EQ(f.active_id(), "");
    const auto m = f.book.get("c1");
    ASSERT_TRUE(m);
    EXPECT_EQ(m.value().gate.regressed_suites, std::vector<std::string>{"coding"});
}

TEST(RsiLoop, StartsFromTheCurrentlyActiveAdapterAndCanBeRolledBackAfterwards) {
    Fixture f;
    // an adapter that was kept in an earlier run
    AdapterManifest m;
    m.adapter_id = "prev";
    m.base_model_sha256 = f.base;
    m.adapter_sha256 = hex('b');
    m.artifact_path = "D:/models/adapters/prev.gguf";
    m.dataset_manifest_sha256 = hex('c');
    m.eval_id = "agentic-test";
    m.baseline = ev(0.5).report.scores();
    m.candidate = ev(0.6).report.scores();
    m.gate = evaluate_gate(m.baseline, m.candidate);
    m.created_at = "2026-11-17T00:00:00Z";
    ASSERT_EQ(m.gate.decision, GateDecision::Keep);
    ASSERT_TRUE(f.book.install(m));
    ASSERT_TRUE(f.book.activate("prev"));
    f.score = {{"prev", 0.60}, {"c1", 0.75}};
    const auto out = run_rsi(f.policy(1), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out) << out.error().message;
    EXPECT_EQ(f.evaluated_ids.front(), "prev")
        << "the baseline is the active adapter, not the bare base model";
    EXPECT_EQ(f.active_id(), "c1");
    ASSERT_TRUE(f.book.rollback(f.base));
    EXPECT_EQ(f.active_id(), "prev");
}

TEST(RsiLoop, NothingFailingStopsImmediatelyAndTrainsNothing) {
    Fixture f;
    RsiHooks h = f.hooks();
    h.evaluate = [](const std::string&) -> Result<Evaluation> { return ev(1.0, {}); };
    const auto out = run_rsi(RsiPolicy{}, f.base, f.book, h, nullptr);
    ASSERT_TRUE(out);
    EXPECT_EQ(f.trained, 0);
    EXPECT_NE(out.value().stop_reason.find("nothing to propose"), std::string::npos);
    EXPECT_EQ(f.installed(), 0u);
}

TEST(RsiLoop, IncomparableCandidateIsInvalidNotInstalledAndNotActivated) {
    Fixture f;
    f.score["c1"] = 0.95;
    f.set["c1"] = 'e';  // evaluated against a different task set
    const auto out = run_rsi(f.policy(1), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out);
    EXPECT_EQ(out.value().cycles[0].decision, GateDecision::Invalid);
    EXPECT_FALSE(out.value().cycles[0].installed);
    EXPECT_NE(out.value().cycles[0].reason.find("not comparable"), std::string::npos);
    EXPECT_EQ(f.installed(), 0u);
    EXPECT_EQ(f.active_id(), "");
}

TEST(RsiLoop, TooFewHeldOutSamplesIsNeverEnoughEvidence) {
    Fixture f;
    RsiHooks h = f.hooks();
    int calls = 0;
    h.evaluate = [&](const std::string&) -> Result<Evaluation> {
        return ++calls == 1 ? ev(0.5, {"parallel"}, 'd', 100) : ev(0.99, {"parallel"}, 'd', 5);
    };
    const auto out = run_rsi(f.policy(1), f.base, f.book, h, nullptr);
    ASSERT_TRUE(out);
    EXPECT_NE(out.value().cycles[0].decision, GateDecision::Keep);
    EXPECT_FALSE(out.value().cycles[0].activated);
    EXPECT_EQ(f.active_id(), "");
}

// ---------------------------------------------------------------------------------------------------------------
// run_rsi: bounds
// ---------------------------------------------------------------------------------------------------------------
TEST(RsiLoop, MaxCyclesIsClampedToTheHardCeiling) {
    Fixture f;
    for (int i = 1; i <= 20; ++i) f.score["c" + std::to_string(i)] = 0.50;  // never improves
    const auto out = run_rsi(f.policy(100, 1000), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out);
    EXPECT_EQ(out.value().cycles.size(), 10u);
    EXPECT_EQ(out.value().stop_reason, "max cycles reached");
    EXPECT_EQ(f.trained, 10);

    Fixture g;
    g.score["c1"] = 0.50;
    const auto one = run_rsi(g.policy(0, 5), g.base, g.book, g.hooks(), nullptr);
    ASSERT_TRUE(one);
    EXPECT_EQ(one.value().cycles.size(), 1u) << "max_cycles below 1 still runs one bounded cycle";
}

TEST(RsiLoop, PatienceBelowOneIsTreatedAsOne) {
    Fixture f;
    f.score["c1"] = 0.50;
    f.score["c2"] = 0.50;
    const auto out = run_rsi(f.policy(5, 0), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out);
    EXPECT_EQ(out.value().cycles.size(), 1u);
}

TEST(RsiLoop, CancelFlagStopsBeforeAnyWork) {
    Fixture f;
    std::atomic<bool> cancel{true};
    const auto out = run_rsi(RsiPolicy{}, f.base, f.book, f.hooks(), &cancel);
    ASSERT_TRUE(out);
    EXPECT_EQ(out.value().stop_reason, "cancelled");
    EXPECT_EQ(f.trained, 0);
}

TEST(RsiLoop, CancelDuringTrainingOrEvaluationInstallsNothing) {
    {
        Fixture f;
        std::atomic<bool> cancel{false};
        RsiHooks h = f.hooks();
        auto train = h.train_candidate;
        h.train_candidate = [&](const std::vector<Proposal>& p, int cycle) {
            cancel = true;  // the user cancels while the trainer runs
            return train(p, cycle);
        };
        f.score["c1"] = 0.99;
        const auto out = run_rsi(f.policy(3), f.base, f.book, h, &cancel);
        ASSERT_TRUE(out);
        EXPECT_EQ(out.value().stop_reason, "cancelled");
        EXPECT_EQ(f.evaluations, 1) << "only the baseline was evaluated";
        EXPECT_EQ(f.installed(), 0u);
        EXPECT_EQ(f.active_id(), "");
    }
    {
        Fixture f;
        std::atomic<bool> cancel{false};
        RsiHooks h = f.hooks();
        auto eval = h.evaluate;
        int calls = 0;
        h.evaluate = [&](const std::string& id) {
            if (++calls == 2)
                cancel = true;  // cancel while the candidate is being evaluated
            return eval(id);
        };
        f.score["c1"] = 0.99;
        const auto out = run_rsi(f.policy(3), f.base, f.book, h, &cancel);
        ASSERT_TRUE(out);
        EXPECT_EQ(out.value().stop_reason, "cancelled");
        EXPECT_EQ(f.installed(), 0u) << "a cancelled loop must not promote";
        EXPECT_EQ(f.active_id(), "");
    }
}

TEST(RsiLoop, WallClockBudgetStopsTheLoop) {
    {
        Fixture f;
        RsiPolicy p = f.policy(5);
        p.budget = std::chrono::milliseconds(0);
        const auto out = run_rsi(p, f.base, f.book, f.hooks(), nullptr);
        ASSERT_TRUE(out);
        EXPECT_EQ(out.value().stop_reason, "wall-clock budget exhausted");
        EXPECT_EQ(f.trained, 0);
    }
    {
        Fixture f;
        f.score["c1"] = 0.99;
        RsiHooks h = f.hooks();
        auto train = h.train_candidate;
        h.train_candidate = [&](const std::vector<Proposal>& p, int cycle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));  // training outlasts the budget
            return train(p, cycle);
        };
        RsiPolicy p = f.policy(5);
        p.budget = std::chrono::milliseconds(40);
        const auto out = run_rsi(p, f.base, f.book, h, nullptr);
        ASSERT_TRUE(out);
        EXPECT_EQ(out.value().stop_reason, "wall-clock budget exhausted");
        EXPECT_EQ(f.installed(), 0u) << "an over-budget candidate is neither gated nor installed";
        EXPECT_EQ(f.active_id(), "");
    }
}

// ---------------------------------------------------------------------------------------------------------------
// run_rsi: hook failures and refusals
// ---------------------------------------------------------------------------------------------------------------
TEST(RsiLoop, TrainingFailureEndsTheLoopWithoutPromotion) {
    Fixture f;
    RsiHooks h = f.hooks();
    h.train_candidate = [](const std::vector<Proposal>&, int) -> Result<Candidate> {
        return Error(ErrorCode::BackendError, "trainer crashed");
    };
    const auto out = run_rsi(RsiPolicy{}, f.base, f.book, h, nullptr);
    ASSERT_TRUE(out);
    EXPECT_NE(out.value().stop_reason.find("trainer crashed"), std::string::npos);
    EXPECT_EQ(f.installed(), 0u);
}

TEST(RsiLoop, EvaluationFailuresEndTheLoopWithoutPromotion) {
    {
        Fixture f;
        RsiHooks h = f.hooks();
        h.evaluate = [](const std::string&) -> Result<Evaluation> {
            return Error(ErrorCode::Timeout, "server down");
        };
        const auto out = run_rsi(RsiPolicy{}, f.base, f.book, h, nullptr);
        ASSERT_TRUE(out);
        EXPECT_NE(out.value().stop_reason.find("baseline evaluation failed"), std::string::npos);
        EXPECT_EQ(f.trained, 0);
    }
    {
        Fixture f;
        RsiHooks h = f.hooks();
        int calls = 0;
        h.evaluate = [&](const std::string&) -> Result<Evaluation> {
            if (++calls == 1)
                return ev(0.6);
            return Error(ErrorCode::Timeout, "server timeout");
        };
        const auto out = run_rsi(RsiPolicy{}, f.base, f.book, h, nullptr);
        ASSERT_TRUE(out);
        EXPECT_NE(out.value().stop_reason.find("server timeout"), std::string::npos);
        EXPECT_EQ(f.installed(), 0u);
        EXPECT_EQ(f.active_id(), "");
    }
}

TEST(RsiLoop, AThrowingHookEndsTheLoopCleanly) {
    {
        Fixture f;
        RsiHooks h = f.hooks();
        h.train_candidate = [](const std::vector<Proposal>&, int) -> Result<Candidate> {
            throw std::runtime_error("boom");
        };
        Result<RsiOutcome> out = make_error(ErrorCode::Internal, "unset");
        EXPECT_NO_THROW({ out = run_rsi(RsiPolicy{}, f.base, f.book, h, nullptr); });
        ASSERT_TRUE(out);
        EXPECT_NE(out.value().stop_reason.find("boom"), std::string::npos);
    }
    {
        Fixture f;
        RsiHooks h = f.hooks();
        h.evaluate = [](const std::string&) -> Result<Evaluation> { throw 42; };
        Result<RsiOutcome> out = make_error(ErrorCode::Internal, "unset");
        EXPECT_NO_THROW({ out = run_rsi(RsiPolicy{}, f.base, f.book, h, nullptr); });
        ASSERT_TRUE(out);
        EXPECT_NE(out.value().stop_reason.find("unknown exception"), std::string::npos);
    }
}

TEST(RsiLoop, ABadCandidateIsRefusedByThePipelineAndNothingIsInstalled) {
    Fixture f;
    f.score["c1"] = 0.9;
    RsiHooks h = f.hooks();
    auto train = h.train_candidate;
    h.train_candidate = [&](const std::vector<Proposal>& p, int cycle) -> Result<Candidate> {
        auto c = train(p, cycle);
        c.value().adapter_sha256 = "not-a-hash";
        return c;
    };
    const auto out = run_rsi(f.policy(2), f.base, f.book, h, nullptr);
    ASSERT_TRUE(out);
    EXPECT_NE(out.value().stop_reason.find("pipeline refused"), std::string::npos);
    EXPECT_EQ(f.installed(), 0u);
    EXPECT_EQ(f.active_id(), "");
}

TEST(RsiLoop, AHookThatReusesAnAdapterIdCannotOverwriteTheAuditTrail) {
    Fixture f;
    f.score["same"] = 0.50;  // never improves, so the loop keeps going
    RsiHooks h = f.hooks();
    auto train = h.train_candidate;
    h.train_candidate = [&](const std::vector<Proposal>& p, int cycle) -> Result<Candidate> {
        auto c = train(p, cycle);
        c.value().adapter_id = "same";
        return c;
    };
    const auto out = run_rsi(f.policy(4, 10), f.base, f.book, h, nullptr);
    ASSERT_TRUE(out);
    ASSERT_EQ(out.value().cycles.size(), 2u);
    EXPECT_NE(out.value().stop_reason.find("pipeline refused"), std::string::npos);
    EXPECT_EQ(f.installed(), 1u);
}

TEST(RsiLoop, UnusableArgumentsAndACorruptBookAreErrors) {
    Fixture f;
    EXPECT_FALSE(run_rsi(RsiPolicy{}, f.base, f.book, RsiHooks{}, nullptr));
    EXPECT_FALSE(run_rsi(RsiPolicy{}, "nothex", f.book, f.hooks(), nullptr));
    EXPECT_FALSE(run_rsi(RsiPolicy{}, "", f.book, f.hooks(), nullptr));
    hcs::test::write_file(f.dir / "train" / "active.json", "{ corrupt");
    const auto out = run_rsi(RsiPolicy{}, f.base, f.book, f.hooks(), nullptr);
    EXPECT_FALSE(out) << "a corrupt active pointer must stop the loop, not be ignored";
    EXPECT_EQ(f.trained, 0);
}

TEST(RsiLoop, RecordsSerialiseToJson) {
    Fixture f;
    f.score["c1"] = 0.75;
    const auto out = run_rsi(f.policy(1), f.base, f.book, f.hooks(), nullptr);
    ASSERT_TRUE(out);
    const json j = to_json(out.value().cycles[0]);
    EXPECT_EQ(j["decision"], "keep");
    EXPECT_EQ(j["adapter_id"], "c1");
    EXPECT_EQ(j["activated"], true);
    EXPECT_EQ(j["proposals"][0]["kind"], "add_training_data");
    EXPECT_EQ(j["proposals"][0]["category"], "parallel");
}
