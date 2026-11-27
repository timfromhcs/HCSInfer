# Salvage review of the two stopped `claude/` worktrees (2026-11-27, task w2-salvage)

Both worktrees were treated read-only. Before any comparison they were copied (without `.git`, `build`, `node_modules`) to
`D:\HCSScratch\backup\flux3-worktree-snapshot-a` (7291 files, 565 MB) and `D:\HCSScratch\backup\repo-analysis-worktree-snapshot-a`
(7407 files). Nothing in the worktrees was deleted, reset or modified. Method: `git status --porcelain -uall` in each worktree,
then per file `Test-Path` + SHA-256 against this branch, then a numstat diff for files present on both sides.

Result: **nothing was ported.** Every compiled/tested file is either superseded by a newer, tested version on this branch or has no test and
no place in the v1.1.0 scope. Per-file decisions follow.

## `claude/flux3-agent-engine-v1-665ab0` (99 uncommitted paths on top of `6a390b8`)

| Area | Files | Decision | Reason |
|---|---|---|---|
| Agent logic | `src/agent/{agent_redact,event_parser,session_manager,tool_audit}.cpp`, matching headers, `src/agent/CMakeLists.txt` | **superseded** | The branch has newer versions of all of them (session_manager 676 vs 641 lines, tool_audit 733 vs 607, event_parser 131 vs 105) with the W2 salvage tests (`5633450`). |
| Agent child process | `include/hcs/agent/process.hpp`, `src/agent/process_win.cpp` | **superseded** | Replaced by the shared `hcs::process` (`src/process/child_process.cpp`, Job Object caps, replaced environment, streaming) behind one launcher seam; a second Job-Object launcher would duplicate it. |
| Train logic | `src/train/{chat_template,dataset_prep,rsi_loop}.cpp` + headers + tests | **superseded** | Newer on this branch (rsi_loop 302 vs 161 lines: reuses `EvalReport`/`evaluate_gate`/`AdapterBook`). |
| Adapter bookkeeping | `include/hcs/train/adapter_store.hpp`, `src/train/adapter_store.cpp`, `tests/unit/train/test_adapter_store.cpp` | **drop** | Same job as `AdapterBook` (`adapter_book.{hpp,cpp}`, 416 lines, gate-checked promotion, tests). Two parallel implementations would diverge. |
| Train child process | `include/hcs/train/process.hpp`, `src/train/process.cpp`, `tests/unit/train/test_process.cpp` | **drop** | Superseded by `hcs::process`; Windows-only, so it would also break the Linux/macOS build of `hcs_train`. |
| Train CLI | `src/cli/train_cmd.{cpp,hpp}`, `src/train/train_main.cpp`, `tests/unit/train/test_train_cli.cpp` | **drop** | The branch has a smaller, tested `train` CLI (`validate/gate/adapter/pipeline`, ADR-012: HCSInfer does not run a trainer). The old one added `train run` which launches the external trainer below; that path has never run. |
| External trainer | `tools/train/{trainer.py,lfm3_gguf.py,verify_parity.py}` | **defer (not in v1.1.0)** | 230+116+83 lines of untested Python needing torch/peft/transformers/gguf (not installed in CI, `jinja2`/`gguf` absent here). "Real trainer run" is NOT RUN in STATUS and excluded from stable scope. Kept in the backup; revisit with a real run and evidence in a later release. |
| Soak | `tests/soak/*`, `scripts/soak.ps1`, `docs/soak.md`, `docs/test-evidence/soak/cpu-quick/*` | **superseded** | W3 versions on this branch are newer (soak_core 862 vs 809 lines, http_storm 429 vs 400). The old evidence is from the older code. |
| Older web demo | `web/src/**` (Vite app), `web/package.json`, `web/index.html`, `web/vercel.json`, `web/scripts/*`, `web/public/favicon.svg` | **drop** | Different, older architecture (Vite + build step, 17 Nov). The branch has the no-build `web/js` app with `npm test` (46 tests) and a different `package.json`; mixing both would break `web/` tests. |

## `claude/repo-analysis-v1-stable-80f8d6` (4 modified, 24 untracked)

| Files | Decision | Reason |
|---|---|---|
| `CMakePresets.json`, `docs/build.md`, `docs/decisions/ADR-008-static-crt-single-exe.md`, `scripts/package.ps1` | **superseded** | The branch already has `windows-msvc-release-static` and `-static-vulkan` presets, a rewritten ADR-008 and package script (numstat shows the branch is 2x larger in each). |
| `docs/test-evidence/W4-static/*`, `docs/test-evidence/W4-vulkan/*` (raw logs, dumpbin, SHA256SUMS for a **0.1.0** build) | **drop** | Evidence for a historical 0.1.0 artefact and an old tree; it cannot support any v1.1.0 claim. Preserved in the backup. |

## Untracked leftovers inside two `wave-c` worktrees (not removed, see below)

`wf_ce2c8be6-718-1` (wave-c/i1): `docs/test-evidence/wave-c/i1/` (a sweep script + one empty image stdout) and `scripts/image-run.ps1`.
`wf_ce2c8be6-718-3` (wave-c/x1): `docs/test-evidence/wave-c/x1/raw/*` (partial FLUX.3 sweep runs, PNGs), `scripts/bench-flux3-sweep.ps1`,
`scripts/bench-flux3-summary.py`, `scripts/bench-image-run.ps1`. They are incomplete experiments without a result table, so they were not
copied. They stay where they are (the worktrees were not removed), so nothing is lost.

## Stale-worktree removal (plain `git worktree remove`, no `--force`)

Removed: `wf_3ff35c84-7c1-2` (wave-b/c1) and `wf_ce2c8be6-718-4` (wave-c/k1). Both were clean; the branches were kept.
Refused and left in place: `wf_3ff35c84-7c1-1` (f1), `-3` (e1), `-4` (b3): "working trees containing submodules cannot be moved or removed";
`wf_ce2c8be6-718-1` and `-3`: untracked files (listed above); `wf_ce2c8be6-718-2` (u1): `git status` fails inside it
(`not a git repository: external/llama.cpp/../../.git/modules/external/llama.cpp`), so it is not known to be clean. The
`wf_5e9ee287-140-*` worktrees did not exist. The owner can remove the remaining ones with `--force` after reviewing this list.
