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
| Chat template | `hcs/train/chat_template.hpp` | Renders the LFM3 chat template (system + `<tools>`, tool calls, generation prompt) and splits a conversation into one prompt/completion example per assistant turn |
| Dataset preparation | `hcs/train/dataset_prep.hpp` | Validate → drop eval-overlapping records → cap per input → render → deterministic train/dev split; writes `train.jsonl`, `dev.jsonl` and a hashed `manifest.json` |
| RSI loop | `hcs/train/rsi_loop.hpp` | Bounded reflect → propose → train (hook) → evaluate → gate loop on top of `EvalReport`, `evaluate_gate`, `run_train_pipeline` and `AdapterBook` |

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
  "artifact_path": "<MODELS>/adapters/lfm3-agentic-0001.gguf",
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

## Chat template and dataset preparation

`render_lfm3` reproduces, for the subset HCS trains on, the Jinja chat template embedded in the LFM3-Omni GGUF:
string content only, no thinking tags, tool calls as `<tool_call><function=NAME><parameter=...>` blocks, the
generation prompt `<|im_start|>assistant
<think></think>`. Anything outside that subset is **rejected with
`Unsupported`**, never rendered differently. Hostile input is bounded: wrongly typed fields are treated as absent
(no exception), JSON nested deeper than 128 levels is refused before anything is copied or dumped (a 100 000-deep
value used to be a stack-overflow path), and a record whose text contains the turn control tokens
(`<|im_start|>`, `<|im_end|>`, `<|startoftext|>`) is not rendered, so a training record cannot forge a turn
boundary. `prepare_dataset` refuses to overwrite an existing `manifest.json`, `train.jsonl` or `dev.jsonl`.

**Parity with Jinja is NOT RUN.** The golden strings in `tests/unit/train/test_chat_template.cpp` were produced by
`tools/train/jinja_reference.py` (the GGUF's own template rendered by Jinja2) by the session that wrote them; that
script needs the `jinja2` and `gguf` Python packages, which are not installed here, so the comparison was not
repeated in this change. Re-run it with
`python -I tools/train/jinja_reference.py --gguf <LFM3-Omni-2B-Q8_0.gguf> --records records.jsonl --out golden.jsonl`
and diff against the C++ renderer before relying on a new template feature.

## RSI loop (`rsi_loop.hpp`)

`run_rsi(policy, base_model_sha256, book, hooks, cancel)`:

1. evaluate the currently active adapter (or the bare base model) with `hooks.evaluate`;
2. `reflect` the failed held-out tasks into bounded `add_training_data` proposals (largest failing category first);
3. `hooks.train_candidate` builds a candidate adapter (the trainer is external; **NOT RUN here**);
4. evaluate the candidate on the same frozen tasks; reports that are not `comparable` (different suite or
   `set_sha256`) are `Invalid` and nothing is installed;
5. otherwise `run_train_pipeline` installs the manifest (an audit trail for `keep`, `rollback` and `invalid`) and
   activates the adapter **only** on a `Keep` gate decision;
6. a kept adapter becomes the baseline of the next cycle; a non-kept one counts towards patience.

Bounds: at most 10 cycles (`max_cycles` is clamped to 1..10), a wall-clock `budget`, `patience` consecutive
non-Keep cycles (at least 1), and a cancel flag checked before the cycle and after training and after evaluation
(a cancelled or over-budget cycle installs nothing). A hook that throws ends the loop cleanly. A hook that reuses an
adapter id cannot overwrite the audit trail (`AlreadyExists` ends the loop). The loop never edits binaries, the
eval set or safety rules. The tests drive it with scripted hooks: they prove control flow and gate discipline, not
model quality, and **no real trainer run exists**.

## Bounds and safety

* Reports, manifests and inputs are read with a byte cap and a JSON nesting cap before parsing.
* Baselines/candidates are only compared when their `eval_id` and `set_sha256` match.
* The gate fails closed; the book refuses promotion of a non-kept adapter; ids cannot escape the book directory.
* No network, no trainer execution, no model download in this module.
* Training data must be legally usable; the validator rejects forbidden provenance (Claude / Anthropic outputs) and
  non-allow-listed licences before anything is rendered.

## Not done / open

* **No real training run**: the LoRA trainer is external (ADR-012) and has not been run on this machine. The
  pipeline is exercised with recorded evaluation reports; `docs/test-evidence/train/` states this.
* The evaluation harness that produces reports from a run (tool-call format validity, BFCL-style accuracy, small
  code tasks) is a later wave; today `tests/eval/agentic/` produces the numbers by hand/script.
* Dedupe across train/held-out sets is not yet enforced by the gate.
