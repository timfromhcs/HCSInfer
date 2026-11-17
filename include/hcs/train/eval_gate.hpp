#pragma once
// Eval gate for HCS Train / RSI (ADR-012): decides whether a trained adapter is kept or rolled back.
//
// Keep ONLY if (a) the held-out score improves by at least `min_heldout_gain`, and (b) no regression suite
// drops by more than `max_suite_drop` (default 0: "does not drop"). Everything else, including malformed or
// incomparable inputs, is Rollback / Invalid - the gate fails closed. Pure function, no I/O.
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace hcs::train {

struct EvalScores {
    double heldout = 0.0;                      // score in [0, 1], higher is better
    std::size_t heldout_samples = 0;           // number of held-out items behind `heldout`
    std::map<std::string, double> regression;  // suite name -> score in [0, 1]; the regression set
};

struct GatePolicy {
    double min_heldout_gain = 0.01;        // absolute gain required on the held-out set
    double max_suite_drop = 0.0;           // tolerated absolute drop per regression suite (0 = none)
    std::size_t min_heldout_samples = 50;  // below this, a gain is noise: not enough evidence
};

enum class GateDecision {
    Keep,      // adapter may be promoted
    Rollback,  // evaluated, but did not earn promotion: keep the previous adapter/base
    Invalid,   // inputs unusable (NaN, out of range, incomparable sets): treat exactly like Rollback
};

struct GateResult {
    GateDecision decision = GateDecision::Invalid;
    std::string reason;                         // human-readable, always set
    double heldout_delta = 0.0;                 // candidate - baseline (0 when Invalid)
    std::vector<std::string> regressed_suites;  // suites that dropped by more than the tolerance
};

const char* to_string(GateDecision d) noexcept;

GateResult evaluate_gate(const EvalScores& baseline, const EvalScores& candidate,
                         const GatePolicy& policy = {});

}  // namespace hcs::train
