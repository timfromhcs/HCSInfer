#pragma once
// Bounded RSI loop for HCS Train (ADR-012, decision 6): reflect on the failures of the last evaluation,
// propose a small data change, let an injected trainer build a candidate adapter, evaluate it on the SAME
// frozen tasks and let the eval gate decide. Everything that records or promotes goes through the existing
// pieces:
//   * EvalReport / comparable()         (eval_report.hpp)  - the one report type; set_sha256 pins the task
//   set
//   * evaluate_gate()                   (eval_gate.hpp)    - Keep / Rollback / Invalid
//   * run_train_pipeline() + AdapterBook (pipeline.hpp, adapter_book.hpp) - manifest, audit trail, activation
// The loop never trains anything itself (the trainer is a hook, NOT RUN in unit tests), never edits binaries,
// the eval set or safety rules, and cannot activate an adapter the gate did not Keep. Hard bounds: max
// cycles, wall-clock budget, patience (consecutive non-Keep cycles) and a cancel flag checked between every
// stage.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "hcs/result.hpp"
#include "hcs/train/adapter_book.hpp"
#include "hcs/train/eval_gate.hpp"
#include "hcs/train/eval_report.hpp"

namespace hcs::train {

struct EvalFailure {
    std::string id, category, reason;
};

// What an evaluation hook returns: the gate-grade report plus, optionally, which held-out tasks failed (the
// report itself deliberately carries only scores).
struct Evaluation {
    EvalReport report;
    std::vector<EvalFailure> failures;
};

// Reads the failed entries of the optional `results` array written by tools/train/eval_suite.py:
//   {"results": [{"id": "...", "category": "...", "pass": false, "reason": "..."}]}
// Missing `results` is fine (no failures known); a wrongly typed entry is an error. Strings are cut to 128
// bytes and stripped of control characters because they end up in proposals and logs. At most 100000 entries
// are read.
Result<std::vector<EvalFailure>> parse_eval_failures(std::string_view json_text);

struct Proposal {
    std::string kind;       // "add_training_data" | "stop"
    std::string category;   // failing eval category (empty for stop)
    std::size_t count = 0;  // suggested number of new training examples of that kind
    std::string rationale;
};

struct RsiPolicy {
    int max_cycles = 3;                                       // hard cap, also clamped to [1, 10]
    std::chrono::milliseconds budget{std::chrono::hours(4)};  // wall clock for the whole loop
    int patience = 2;                       // consecutive non-Keep cycles before stopping (>= 1)
    std::size_t examples_per_failure = 40;  // proposal sizing: new examples per failed task of a category
    std::size_t max_examples_per_category = 400;
    GatePolicy gate;
};

// What the training hook produced. It never writes into the AdapterBook itself: the loop installs the
// manifest.
struct Candidate {
    std::string adapter_id;      // must be new; the book refuses a duplicate
    std::string adapter_sha256;  // 64 hex
    std::string artifact_path;
    std::string dataset_manifest_sha256;  // 64 hex
    std::map<std::string, std::string> hyperparameters;
    std::string created_at;  // ISO-8601 UTC supplied by the hook (the loop holds no wall clock)
};

struct RsiHooks {
    // Evaluates the base model (empty id) or the adapter with that id; must use the same frozen tasks every
    // time.
    std::function<Result<Evaluation>(const std::string& adapter_id)> evaluate;
    // Trains a candidate for the given proposals. NOT RUN in unit tests (no trainer in CI).
    std::function<Result<Candidate>(const std::vector<Proposal>& proposals, int cycle)> train_candidate;
};

struct CycleRecord {
    int cycle = 0;
    std::vector<Proposal> proposals;
    std::string adapter_id;
    GateDecision decision = GateDecision::Invalid;
    std::string reason;
    double baseline_heldout = 0.0;
    double candidate_heldout = 0.0;
    bool installed = false;  // a manifest (the audit trail) was written
    bool activated = false;  // the adapter became the active one
};

struct RsiOutcome {
    std::vector<CycleRecord> cycles;
    std::string stop_reason;
    std::string final_adapter;  // active adapter id after the loop ("" = base model)
};

// Pure: turns failures into bounded data proposals (largest failing category first, ties by name), or a
// single "stop" proposal when nothing failed.
std::vector<Proposal> reflect(const std::vector<EvalFailure>& failures, const RsiPolicy& policy);

// Runs the loop for one base model. Returns an error only for unusable arguments or an unreadable
// AdapterBook; every other end (hook failure, cancel, budget, patience, refused install) is reported in
// RsiOutcome::stop_reason.
Result<RsiOutcome> run_rsi(const RsiPolicy& policy, const std::string& base_model_sha256, AdapterBook& book,
                           const RsiHooks& hooks, const std::atomic<bool>* cancel);

nlohmann::json to_json(const Proposal& p);
nlohmann::json to_json(const CycleRecord& c);

}  // namespace hcs::train
