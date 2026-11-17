# Evidence: HCS Train pipeline (ADR-012, V1.1 wave 2)

Date: 2026-11-17. Machine: Windows 11 26200, Ryzen 7 7735HS, MSVC 19.44.35228 x64, CMake 4.4, Ninja,
clang-format/clang-tidy 19.1.5 (VS 2022-bundled). Build: `windows-msvc-release`.

## What was implemented

`hcs_train` gained the evaluation-report ingestion, the adapter book and the pipeline on top of the existing
dataset validator and eval gate: `include/hcs/train/{eval_report,adapter_book,pipeline}.hpp`,
`src/train/{eval_report,adapter_book,pipeline}.cpp`, and the `HCSInfer train ...` CLI
(`src/cli/train_cmd.{hpp,cpp}`). See `docs/train.md`.

## Results

| Gate | Status | Evidence |
|---|---|---|
| Release build after the change | **PASS** | build log this run (`build/windows-msvc-release`) |
| Full Release CTest | **PASS 505/505, 0 failed** (39 s) | was 475 before; +25 unit +5 contract |
| `train` unit tests (`EvalReport.*`, `AdapterBook.*`, `TrainPipeline.*`) | **PASS 25/25** | `ctest -R "EvalReport|AdapterBook|TrainPipeline"` |
| `train` CLI contract tests (`Cli.Train*`) | **PASS 5/5** | `ctest -R "Cli\."` (18 contract tests total, all green) |
| clang-format | **PASS** | `scripts/format.ps1` - 164 files |
| clang-tidy | **PASS** | `scripts/static-analysis.ps1 -Scope src -FailOnFindings` - 55 TUs, 0 diagnostics |
| Real CLI walkthrough (validate/gate/pipeline/adapters) | **PASS** | `cli-demo.txt` in this directory |
| **Real LoRA training run** | **NOT RUN** | needs the external trainer (ADR-012); this module never runs one |
| **Real adapter artifact promotion** | **NOT RUN** | no trained adapter exists on this machine |

### CLI walkthrough (from `cli-demo.txt`)

* `train validate` on a two-record set: **1 accepted, 1 rejected** (`forbidden_provenance` - a record generated
  by a Claude-family model is refused, ADR-012 item 3). Exit 0.
* `train gate` on the recorded agentic numbers (0.925 baseline vs 0.95 candidate, 40 held-out items): **keep**.
* `train pipeline` (keep): manifest installed **and activated** (`lfm3-agentic-0001`).
* `train pipeline` (worse candidate): **rollback**, installed for the audit trail, **not** activated.
* `train adapter list`: `lfm3-agentic-0001` active, `lfm3-agentic-0002` inactive.
* `train adapter activate lfm3-agentic-0002` (**refused**, exit 11, `invalid_state`): a rollback-gated adapter
  cannot be promoted.
* `train adapter rollback --base <sha>`: the pointer moves back; both manifests still exist.

## What this proves and what it does not

* **Proves**: data validation, report comparability, the gate, the adapter book (install/list/activate/rollback,
  promotion refused unless kept, nothing deleted), the pipeline decision and the CLI, all with bounded,
  fail-closed parsing and path-safe ids.
* **Does not prove**: that any adapter improves the real model. That needs a trainer run and a real held-out
  evaluation; both are **NOT RUN** here. The numbers used above are the recorded `agentic-lfm3` baseline and a
  synthetic candidate, not a trained result.

## Reproduce

```powershell
. .\scripts\common.ps1 ; Initialize-MsvcEnvironment
cmake --preset windows-msvc-release ; cmake --build --preset windows-msvc-release --parallel
ctest --preset windows-msvc-release -R "EvalReport|AdapterBook|TrainPipeline|Cli\."
```

## Files

* `cli-demo.txt` - the real CLI walkthrough above (commands, outputs and exit codes).
