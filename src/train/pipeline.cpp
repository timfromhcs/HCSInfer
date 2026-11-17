#include "hcs/train/pipeline.hpp"

#include <utility>

#include "train_util.hpp"

namespace hcs::train {

namespace {

Result<PipelineOutcome> invalid(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

}  // namespace

Result<PipelineOutcome> run_train_pipeline(const PipelineRequest& req, AdapterBook& book) {
    if (!detail::valid_adapter_id(req.adapter_id)) {
        return invalid("adapter_id must be a safe identifier (max 64, [A-Za-z0-9._-])");
    }
    if (!detail::is_hex64(req.base_model_sha256)) {
        return invalid("base_model_sha256 must be 64 hex characters");
    }
    if (!detail::is_hex64(req.adapter_sha256)) {
        return invalid("adapter_sha256 must be 64 hex characters");
    }
    if (!detail::is_hex64(req.dataset_manifest_sha256)) {
        return invalid("dataset_manifest_sha256 must be 64 hex characters");
    }
    if (req.artifact_path.empty() || req.artifact_path.size() > 4096) {
        return invalid("artifact_path must be a non-empty path (max 4096 bytes)");
    }
    if (req.created_at.empty() || req.created_at.size() > 64) {
        return invalid("created_at must be a non-empty timestamp string");
    }

    std::string reason;
    if (!comparable(req.baseline, req.candidate, reason)) {
        return invalid("baseline and candidate are not comparable: " + reason);
    }

    AdapterManifest manifest;
    manifest.adapter_id = req.adapter_id;
    manifest.base_model_sha256 = req.base_model_sha256;
    manifest.adapter_sha256 = req.adapter_sha256;
    manifest.artifact_path = req.artifact_path;
    manifest.dataset_manifest_sha256 = req.dataset_manifest_sha256;
    manifest.eval_id = req.baseline.eval_id;
    manifest.hyperparameters = req.hyperparameters;
    manifest.baseline = req.baseline.scores();
    manifest.candidate = req.candidate.scores();
    manifest.gate = evaluate_gate(manifest.baseline, manifest.candidate, req.policy);
    manifest.created_at = req.created_at;

    auto installed = book.install(manifest, /*overwrite=*/false);
    if (!installed) {
        return installed.error();
    }

    PipelineOutcome outcome;
    outcome.adapter_id = manifest.adapter_id;
    outcome.gate = manifest.gate;
    outcome.installed = true;
    outcome.manifest_path = (book.root() / "adapters" / manifest.adapter_id / "manifest.json").string();

    if (manifest.gate.decision == GateDecision::Keep) {
        auto activated = book.activate(manifest.adapter_id);
        if (!activated) {
            return activated.error();
        }
        outcome.activated = true;
    }
    return outcome;
}

}  // namespace hcs::train
