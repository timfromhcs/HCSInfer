# HCS Train: dataset validation, eval gate, adapter bookkeeping (ADR-012)

Status: **implemented** for everything HCS owns. HCS Train does **not** run a trainer (ADR-012: a trainer is an
external job); it validates training data, ingests evaluation reports, applies the keep/rollback gate and keeps
versioned adapter manifests with a safe active pointer. A real LoRA run on this machine is **NOT RUN**.

Module: `hcs_train` (`include/hcs/train/`, `src/train/`). CLI: `HCSInfer train ...`. Evidence:
`docs/test-evidence/train/`.

## Pieces

| Piece | Header | Responsibility |
|---|---|---|
| Dataset validator | `hcs/train/dataset_validator.hpp` | JSONL schema, required `licence`/`source`/`generator`, licence allow-list, forbidden provenance, secret scan, duplicates — bounded and fail-closed |
| Evaluation report | `hcs/train/eval_report.hpp` | Parse one evaluation report (set identity + scores); a baseline and a candidate are only *comparable* with the same `eval_id` and `set_sha256` |
| Eval gate | `hcs/train/eval_gate.hpp` | Pure decision: `Keep` only if the held-out score improves by ≥ `min_heldout_gain` **and** no regression suite drops by more than `max_suite_drop`; everything else is `Rollback`/`Invalid` |
| Adapter book | `hcs/train/adapter_book.hpp` | One immutable manifest per adapter + an `active.json` pointer per base model; promotion is a pointer move, rollback moves it back, nothing is deleted |
| Pipeline | `hcs/train/pipeline.hpp` | Validate → gate → record a manifest for *every* decision → promote only on `Keep` |

## Evaluation report format

One report per model against a frozen set (the harness must reuse the same prompts/seed for baseline and
candidate; ADR-012 item 4):

```json
{
  "schema_version": 1,
  "eval_id": "agentic-lfm3",
  "set_sha256": "<64 hex: the frozen task set>",
  "heldout": { "samples": 40, "score": 0.925 },
  "regression": [
    { "name": "coding",    "samples": 8,  "score": 1.0 },
    { "name": "tool_use",  "samples": 20, "score": 0.906 }
  ]
}
```

`heldout_samples < min_heldout_samples` is `Invalid` (a gain on too few items is noise). Missing, empty or
different regression suites are `Invalid`. Scores must be finite numbers in `[0, 1]`.

## Adapter book layout

Under `<state-dir>/train/` (`%LOCALAPPDATA%\HCSInfer\train` by default):

```
train/
├── active.json                      # { schema_version, targets: { "<base_sha256>": { active, previous } } }
└── adapters/
    └── <adapter-id>/manifest.json   # immutable record of one training result
```

`manifest.json` records the base model hash, the adapter artifact hash and path, the dataset manifest hash, the
eval id, hyper-parameters, the baseline and candidate scores and the **gate result**. Rules:

* `install` writes a manifest; a duplicate id is refused unless overwritten explicitly.
* `activate` is refused unless the manifest's gate decision is `Keep` (error `invalid_state`) — a bad adapter
  cannot be promoted through the API even if a caller asks.
* `rollback` swaps the pointer back to the previous adapter; with no previous it deactivates. Nothing is deleted,
  so every earlier adapter stays available.
* Adapter ids must be safe single path components (`[A-Za-z0-9._-]`, ≤ 64, no `..`, not a Windows device name).

## CLI

```powershell
HCSInfer train validate --dataset data.jsonl [--json]
HCSInfer train gate --baseline base.json --candidate cand.json [--min-gain 0.01] [--max-drop 0.0] [--min-samples 50] [--json]
HCSInfer --state-dir <dir> train adapter list [--json]
HCSInfer --state-dir <dir> train adapter show <id>
HCSInfer --state-dir <dir> train adapter activate <id>
HCSInfer --state-dir <dir> train adapter rollback --base <sha256>
HCSInfer --state-dir <dir> train pipeline --input result.json [--json]
```

`train --help` works at every level. A `train gate`/`train pipeline` run that decides `rollback` or `invalid` is a
**successful** evaluation (exit 0): the decision is data. Errors use the project exit codes (2 invalid argument,
7 not found, 11 invalid state — activating a non-kept adapter, 12 already exists, 8 I/O).

### Pipeline input

```json
{
  "adapter_id": "lfm3-agentic-0001",
  "base_model_sha256": "<64 hex>",
  "adapter_sha256": "<64 hex>",
  "artifact_path": "D:/HCSModels/adapters/lfm3-agentic-0001.gguf",
  "dataset_manifest_sha256": "<64 hex>",
  "hyperparameters": { "rank": "8", "lr": "1e-4" },
  "baseline":  { "...the baseline evaluation report..." },
  "candidate": { "...the candidate evaluation report..." },
  "gate_policy": { "min_heldout_gain": 0.01, "max_suite_drop": 0.0, "min_heldout_samples": 50 }
}
```

The pipeline records a manifest for `keep`, `rollback` and `invalid` alike (an audit trail) and activates only on
`keep`. An incomparable baseline/candidate is rejected before anything is written.

## RSI loop mapping (ADR-012 item 6)

`collect -> validate (DatasetValidator) -> train (external, user-started) -> evaluate (external harness) ->
gate (evaluate_gate) -> keep/rollback (AdapterBook)`. The holdout and regression sets are frozen before training;
the training data from the agent's own runs is admitted only after an executable check passed and the
`DatasetValidator` accepted it. Nothing here modifies the runtime binaries or the policy layer.

## Bounds and safety

* Reports, manifests and inputs are read with a byte cap and a JSON nesting cap before parsing.
* Baselines/candidates are only compared when their `eval_id` and `set_sha256` match.
* The gate fails closed; the book refuses promotion of a non-kept adapter; ids cannot escape the book directory.
* No network, no trainer execution, no model download in this module.

## Not done / open

* **No real training run**: the LoRA trainer is external (ADR-012) and has not been run on this machine. The
  pipeline is exercised with recorded evaluation reports; `docs/test-evidence/train/` states this.
* The evaluation harness that produces reports from a run (tool-call format validity, BFCL-style accuracy, small
  code tasks) is a later wave; today `tests/eval/agentic/` produces the numbers by hand/script.
* Dedupe across train/held-out sets is not yet enforced by the gate.
