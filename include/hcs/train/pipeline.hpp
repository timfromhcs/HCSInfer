#pragma once
// HCS Train pipeline (ADR-012): from a training result and two evaluations to a kept-or-rolled-back adapter.
//
// This module owns the decision and the bookkeeping only. It does NOT run a trainer (ADR-012: HCS does not
// reimplement a trainer). The caller supplies the trainer's output identity (adapter hash + artifact path)
// and the two evaluation reports; the pipeline gates them, records a manifest for every decision (an audit
// trail, including Rollback/Invalid) and promotes only when the gate says Keep.
#include <map>
#include <string>

#include "hcs/result.hpp"
#include "hcs/train/adapter_book.hpp"
#include "hcs/train/eval_gate.hpp"
#include "hcs/train/eval_report.hpp"

namespace hcs::train {

struct PipelineRequest {
    std::string adapter_id;
    std::string base_model_sha256;
    std::string adapter_sha256;
    std::string artifact_path;
    std::string dataset_manifest_sha256;
    std::map<std::string, std::string> hyperparameters;
    EvalReport baseline;
    EvalReport candidate;
    GatePolicy policy;
    std::string created_at;
};

struct PipelineOutcome {
    std::string adapter_id;
    GateResult gate;
    bool installed = false;
    bool activated = false;
    std::string manifest_path;
};

Result<PipelineOutcome> run_train_pipeline(const PipelineRequest& request, AdapterBook& book);

}  // namespace hcs::train
