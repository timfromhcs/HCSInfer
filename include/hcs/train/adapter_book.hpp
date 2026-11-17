#pragma once
// Versioned adapter bookkeeping for HCS Train (ADR-012 item 5).
//
// The book stores one immutable manifest per trained adapter and a small pointer file that records which
// adapter is active per base model. Promotion is a pointer move; rollback moves it back. Neighbouring
// manifests and the previous adapter are never deleted. Activation is refused unless the eval gate said
// `Keep`, so a bad adapter cannot be promoted through this API even if a caller asks for it.
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/train/eval_gate.hpp"

namespace hcs::train {

struct AdapterManifest {
    int schema_version = 1;
    std::string adapter_id;               // safe single path component, e.g. "lfm3-agentic-0001"
    std::string base_model_sha256;        // the base model the adapter targets
    std::string adapter_sha256;           // hash of the adapter artifact
    std::string artifact_path;            // recorded location of the artifact (the book does not copy it)
    std::string dataset_manifest_sha256;  // hash of the validated dataset manifest
    std::string eval_id;                  // the evaluation suite used
    std::map<std::string, std::string> hyperparameters;
    EvalScores baseline;
    EvalScores candidate;
    GateResult gate;
    std::string created_at;  // ISO-8601 UTC, supplied by the caller (pure logic holds no clock)
};

std::string to_json(const AdapterManifest& manifest);
Result<AdapterManifest> parse_adapter_manifest(std::string_view json_text);

class AdapterBook {
public:
    explicit AdapterBook(std::filesystem::path root);

    const std::filesystem::path& root() const noexcept { return root_; }

    // Writes adapters/<id>/manifest.json. An existing id fails with AlreadyExists unless `overwrite`.
    Result<void> install(const AdapterManifest& manifest, bool overwrite = false);
    Result<std::vector<AdapterManifest>> list() const;
    Result<AdapterManifest> get(const std::string& adapter_id) const;

    // The adapter active for a base model, or nullopt when none is active.
    Result<std::optional<AdapterManifest>> active(const std::string& base_model_sha256) const;

    // Promote `adapter_id` for its base model. Refused unless the manifest's gate decision is Keep.
    Result<void> activate(const std::string& adapter_id);

    // Move the pointer back to the previous adapter (or deactivate when there is none). Never deletes.
    Result<void> rollback(const std::string& base_model_sha256);

private:
    std::filesystem::path manifest_path(const std::string& adapter_id) const;
    std::filesystem::path pointer_path() const;

    std::filesystem::path root_;
};

}  // namespace hcs::train
