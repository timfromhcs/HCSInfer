#include "hcs/train/eval_gate.hpp"

#include <cmath>
#include <utility>
#include <sstream>

namespace hcs::train {

namespace {

constexpr double kEps = 1e-12;  // absorbs binary floating-point noise on thresholds, nothing more

bool valid_score(double v) {
    return std::isfinite(v) && v >= 0.0 && v <= 1.0;
}

GateResult invalid(std::string reason) {
    GateResult r;
    r.decision = GateDecision::Invalid;
    r.reason = std::move(reason);
    return r;
}

// Returns an empty string when the scores are well formed.
std::string check_scores(const char* who, const EvalScores& s) {
    if (!valid_score(s.heldout))
        return std::string(who) + " held-out score is not a finite number in [0, 1]";
    for (const auto& [suite, score] : s.regression) {
        if (suite.empty())
            return std::string(who) + " has a regression suite with an empty name";
        if (!valid_score(score))
            return std::string(who) + " regression suite '" + suite +
                   "' score is not a finite number in [0, 1]";
    }
    return {};
}

}  // namespace

const char* to_string(GateDecision d) noexcept {
    switch (d) {
        case GateDecision::Keep:
            return "keep";
        case GateDecision::Rollback:
            return "rollback";
        case GateDecision::Invalid:
            return "invalid";
    }
    return "invalid";
}

GateResult evaluate_gate(const EvalScores& baseline, const EvalScores& candidate, const GatePolicy& policy) {
    if (!valid_score(policy.min_heldout_gain) || !valid_score(policy.max_suite_drop))
        return invalid("gate policy thresholds must be finite numbers in [0, 1]");
    if (auto e = check_scores("baseline", baseline); !e.empty())
        return invalid(std::move(e));
    if (auto e = check_scores("candidate", candidate); !e.empty())
        return invalid(std::move(e));

    if (baseline.regression.empty())
        return invalid("baseline has no regression suites: a regression-free claim cannot be proven");
    if (baseline.heldout_samples != candidate.heldout_samples)
        return invalid("baseline and candidate were scored on held-out sets of different sizes");
    if (baseline.heldout_samples < policy.min_heldout_samples) {
        std::ostringstream os;
        os << "held-out set has " << baseline.heldout_samples << " samples, at least "
           << policy.min_heldout_samples << " are required";
        return invalid(os.str());
    }
    // The regression set must be identical: a candidate that silently omits a suite must not pass.
    for (const auto& [suite, score] : baseline.regression) {
        (void)score;
        if (candidate.regression.find(suite) == candidate.regression.end())
            return invalid("candidate is missing regression suite '" + suite + "'");
    }
    for (const auto& [suite, score] : candidate.regression) {
        (void)score;
        if (baseline.regression.find(suite) == baseline.regression.end())
            return invalid("candidate has regression suite '" + suite + "' that the baseline lacks");
    }

    GateResult r;
    r.heldout_delta = candidate.heldout - baseline.heldout;
    for (const auto& [suite, base_score] : baseline.regression) {
        const double drop = base_score - candidate.regression.at(suite);
        if (drop > policy.max_suite_drop + kEps)
            r.regressed_suites.push_back(suite);
    }

    std::ostringstream os;
    if (!r.regressed_suites.empty()) {
        r.decision = GateDecision::Rollback;
        os << "regression suite dropped:";
        for (const auto& s : r.regressed_suites) os << ' ' << s;
    } else if (r.heldout_delta + kEps < policy.min_heldout_gain || r.heldout_delta <= 0.0) {
        r.decision = GateDecision::Rollback;
        os << "held-out gain " << r.heldout_delta << " is below the required " << policy.min_heldout_gain;
    } else {
        r.decision = GateDecision::Keep;
        os << "held-out improved by " << r.heldout_delta << " and no regression suite dropped";
    }
    r.reason = os.str();
    return r;
}

}  // namespace hcs::train
