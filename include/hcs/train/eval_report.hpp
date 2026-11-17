#pragma once
// Evaluation-report ingestion for HCS Train (ADR-012).
//
// An evaluation harness (for example `tests/eval/agentic/score.py`, or a future runner) writes ONE report
// per model, evaluated against a frozen set. The report carries the identity of that set so a baseline and
// a candidate can be proven comparable, and the scores the eval gate needs. Reports are untrusted input:
// parsing is bounded and anything unexpected is rejected with a precise reason (fail closed).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "hcs/result.hpp"
#include "hcs/train/eval_gate.hpp"

namespace hcs::train {

struct RegressionSuiteResult {
    double score = 0.0;  // in [0, 1], higher is better
    std::size_t samples = 0;
};

struct EvalReport {
    int schema_version = 1;
    std::string eval_id;     // identity of the evaluation suite, e.g. "agentic-lfm3"
    std::string set_sha256;  // 64 hex characters: the frozen task set
    std::size_t heldout_samples = 0;
    double heldout_score = 0.0;
    std::map<std::string, RegressionSuiteResult> regression;

    // The scores the gate consumes.
    EvalScores scores() const;
};

// Parse a report from JSON text. On any defect returns InvalidArgument with the reason.
Result<EvalReport> parse_eval_report(std::string_view json_text);

// Parse from a file, reading at most `max_bytes` (bounded allocation).
Result<EvalReport> parse_eval_report_file(const std::filesystem::path& path,
                                          std::uint64_t max_bytes = 1u << 20);

std::string to_json(const EvalReport& report);

// Comparable = same evaluation id and the same frozen set hash. `reason` is set when they are not.
bool comparable(const EvalReport& baseline, const EvalReport& candidate, std::string& reason);

}  // namespace hcs::train
