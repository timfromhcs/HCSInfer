#pragma once
// `HCSInfer train prepare`: validated, deduplicated, eval-clean, chat-template-rendered SFT data (ADR-012).
//
// Pipeline per input JSONL: DatasetValidator (licence allow-list, forbidden generator, secrets, size caps,
// duplicates) -> drop records whose user text appears in a held-out eval file -> per-input row cap -> render
// with the LFM3 chat template into one SFT example per assistant turn -> deterministic train/dev split by id
// hash. Output directory (never inside the model folder, never overwrites an existing manifest):
//   train.jsonl, dev.jsonl   {"id","prompt","completion","licence","source","generator"} per line
//   manifest.json            inputs (sha256, counts, licences), policy, outputs (sha256, counts), attribution
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/train/dataset_validator.hpp"

namespace hcs::train {

struct PrepareOptions {
    std::vector<std::filesystem::path> inputs;        // validated conversation JSONL files (required)
    std::filesystem::path out_dir;                    // created; must not already hold a manifest.json
    std::vector<std::filesystem::path> eval_files;    // held-out task files (tests/eval/**): never trained on
    std::vector<std::string> extra_allowed_licences;  // e.g. "ODC-By-1.0" after the research-note review
    std::size_t max_records_per_input = 5000;         // cap per input file (after validation)
    double dev_fraction = 0.05;                       // [0, 0.5]; deterministic split by id hash
    std::uint64_t seed = 1;                           // salts the split hash
    DatasetLimits limits;
};

struct PrepareInputReport {
    std::string path;
    std::string sha256;
    ValidationReport validation;
    std::size_t eval_overlap_dropped = 0;
    std::size_t capped = 0;         // records dropped by max_records_per_input
    std::size_t render_failed = 0;  // records the chat template could not represent
    std::size_t examples = 0;       // SFT examples produced from this input
};

struct PrepareReport {
    std::vector<PrepareInputReport> inputs;
    std::size_t train_examples = 0;
    std::size_t dev_examples = 0;
    std::string train_sha256, dev_sha256;
    std::filesystem::path manifest_path;
};

// Rejects (Result error) when any input has zero usable examples, when an input is missing, or when out_dir
// already holds a manifest. Per-record problems are counted in the report, never silently ignored.
Result<PrepareReport> prepare_dataset(const PrepareOptions& opt);

// Normalised user prompts of held-out task files (a JSONL of {"messages": [...]}); used for the overlap
// check.
Result<std::vector<std::string>> load_eval_prompts(const std::vector<std::filesystem::path>& files);

}  // namespace hcs::train
