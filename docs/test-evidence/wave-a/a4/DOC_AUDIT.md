# Documentation vs reality audit (wave A, a4)

Base: `main` at `52ea9e7` plus the wave-A worktree branch `wave-a/a4`. Reality sources: the freshly built CPU EXE
(`HCSInfer 1.0.0-rc.1`, `--help` output saved in `cli-help/`), the source tree, `gh run list` output
(`github-ci-state.txt`), the fresh CTest runs (`ctest-*.txt`) and `doctor --json` (`doctor-cpu-build.json`).
Line numbers are those of the file as found on `main` (before the fixes in this branch). "Fixed" means the fix is in this
branch's diff; "Open" means proposed only.

## Claims that were wrong or stale

| # | File : line | Claim | Reality | Fix | State |
|---|---|---|---|---|---|
| 1 | README.md:7 | CI badge "not yet run on this revision" | `pr-quick-gate` is success on `0853143` (run 44852149002) | badge now says green on `v1.0.0-rc.1` | Fixed |
| 2 | README.md:41-44 | "Nothing is published yet" | tag `v1.0.0-rc.1` is pushed (`git ls-remote`), the hosted `release` workflow (run 44849519815, `40bf581`) produced a **draft** pre-release with 8 assets | text rewritten (draft, not public, no PyPI) | Fixed |
| 3 | README.md:143-144 | CLI example renders `--width 256 --height 256 --steps 2` | 256 px / 2 steps is a diagnostic size and produces junk with SD 1.5 (wave A image root-cause work); `image generate --help` documents 512 / 20 as defaults | example uses 512 / 20 | Fixed |
| 4 | README.md:267-268 | NOT RUN list includes "a hosted run of the release workflow" | the hosted run happened (green); what is still NOT RUN is a clean-machine run of the hosted artifacts | reworded | Fixed |
| 5 | docs/api.md:164-166 | "The engine cannot report intermediate images or percentages" | the diffusion adapter has a per-step progress sink; the HTTP route just does not expose it | reworded (engine has it, route does not) | Fixed |
| 6 | docs/api.md:149-156 | image route table lists no `preset` | `src/api/http_api.cpp:953,1029,1112` accept `preset` (`fast`/`quality`) and return it in `hcs.preset`; CLI `--preset` documented in `image generate --help` | `preset` row + response field added | Fixed |
| 7 | docs/gui.md:132 | "the engine reports none" (per-step progress) | same as 5 | reworded | Fixed |
| 8 | docs/roadmap/STATUS.md:3, 17, 21-26, 43-52, 122-123, 159-160, 240, 271, 291, 305, 307 | CI "unknown (never pushed)", release workflow "NOT RUN", "not tagged/merged/pushed" | the tag is pushed and an ancestor of `main`; `claude/repo-analysis-v1-stable-d7c553` is an ancestor of `main`; `pr-quick-gate` green, `release` green (see 2); `hardware.yml` has never completed (4 cancelled, 1 pending) | each stale phrase replaced with the real state or a "superseded" pointer; new "Wave A baseline" section | Fixed |
| 9 | docs/packaging.md:78 | "Hosted-runner results of `release.yml` are NOT RUN: it has not been pushed" | pushed, green | reworded | Fixed |
| 10 | docs/release-notes-1.0.0-rc.1.md:3, CHANGELOG.md:7 | "not tagged, not merged, not pushed" | tagged, pushed, merged, release is a draft | reworded | Fixed |
| 11 | docs/test-evidence README of the e2e agentic test / `tests/eval/agentic/e2e_lfm3.py` | (not a doc claim) the real-model test `e2e_llama_agentic_real` passed its 18 checks but FAILED on a fresh checkout | the evidence directory (`artifacts/e2e-lfm3`) was created only after the eval, but the eval writes its JSON there first (`FileNotFoundError`); it had only passed before because the dir existed from earlier runs | `os.makedirs(evidence)` at start; re-run recorded in `ctest-agentic-fix.txt` | Fixed (red -> green) |

## Claims that were checked and are correct

* Exit codes: `models load x`, `models unload` and `tune` exit **10**; `models inspect <unknown>` exits 7; missing args on `chat`, `vision`,
  `image`, `cache clear` and an unknown command exit 2 (run against the built EXE, see `cli-exit-codes.txt`).
  README:106/243/266, STATUS, `docs/release-notes` and the CLI help text agree.
* Pins in `docs/supported-models.md` and STATUS (`d3c3e24`, `532d2c1`, prime-agent `8980e66`) equal `external/dependency-lock.json`.
* Hardware / driver / OS values in `docs/support-matrix.md` (Ryzen 7 7735HS, 0x1681, driver `32.0.21043.12001`, OS build 26200.9974, 21,245,952,000 B RAM)
  equal the fresh `doctor --json` of this run. Toolchain: MSVC 19.44.35228 (configure logs).
* README benchmark ratios (2.8x, 7.8x, 7.3x, 5x) match the table values; FLUX 508 / 566 s is consistent with `docs/benchmarks.md`.
* README / supported-models agentic eval baseline 37/40 (tool use 0.906, coding 1.0, parallel 0/3) was reproduced in this baseline run
  (`ctest-cpu-phaseB.txt`).
* `python -I tools/check_links.py`: 86 markdown files, 82 relative links, 0 broken.
* GUI: 8 tabs in `src/gui/assets/index.html`, Agent/Train panels labelled Planned, no LoRA control (gui.md:26, 32-33).
* `docs/test-evidence/` directories referenced by `docs/support-matrix.md` all exist (M0, M1, M2, M2-vision, M3-image, M3-api-mcp,
  B7-params, vulkan-kernels, lfm3, wave-a/b/c/f/g, benchmark(s), train, agent, web, release-final).

## Open items (unproven, inconsistent or needing an owner decision)

| # | File : line | Claim / problem | Proposed fix |
|---|---|---|---|
| O1 | docs/supported-models.md:20-21 vs README.md:182-183 | LFM3-Omni numbers differ (35.9 t/s, 438 ms, 18.8 t/s, 2,790 ms vs 34.5, 393, 18.1, 3,070): two different runs, only the README gives the provenance | add "(lfm3 wave, single run)" vs "(wave D, median of 5)" labels, or reference one table |
| O2 | docs/roadmap/STATUS.md:76 (gate row), 274-277 | test counts (815 / 811 passed / 4 skipped; 816 / 812) predate this baseline (816 registered) | superseded by the new "Wave A baseline" section; rewrite the row in wave G (G3) |
| O3 | docs/roadmap/STATUS.md (whole file, 320 lines) | long, many historical rows ("wave R", "W1".."W6") hard to read; several rows still say "uncommitted", "not yet" | restructure in wave G (G3): one current table, history moved to CHANGELOG |
| O4 | docs/api.md:200-201 | "A real-model run of `POST /hcs/v1/benchmarks` is NOT RUN" | still true (no e2e test calls it with a model: `tests/e2e/api_mcp_real` does not reference `benchmarks`); add a real-model e2e step in wave E/F |
| O5 | docs/gui.md:138 | "Model `kind` is reported as `text` for vision models" while `src/core/model_registry.cpp:38` has a `vision` kind | verify; if the kind exists, update the sentence or the UI selector |
| O6 | tests/e2e/gui_real (CTest `e2e_gui_xss`) | SKIPPED (NOT RUN) in this baseline: the CMake-selected Python 3.14 has no `websockets` package. The same test passes with another Python that has it (`xss-direct.txt`, 8/8 PASS, exit 0) | wave G (G2): document/provision a dev venv on D: and set `HCS_E2E_PYTHON_PATH`, so the gate is not silently skipped |
| O7 | README.md:104 / STATUS | "HCS Train, HCS Agent: Implemented (logic)" is accurate, but the CLI help lists `train` without saying a real training run is NOT RUN | acceptable; revisit when wave D lands |
| O8 | docs/support-matrix.md | no row for CI / hosted-runner status and no `hardware.yml` state | add a small "Automation" table in G3 |
| O9 | AGENTS.md section 26.7 / README | web demo (Vercel) is described, no deployment URL or commit is recorded in STATUS.md | record or mark NOT RUN |

## Stray files (report only; nothing was deleted)

In the shared checkout `D:\HCSInfer` (not in this worktree):

* `D:\HCSInfer\cpu-256x256-2step.png` (115,863 B, 2026-11-17) and `D:\HCSInfer\cpu-edit-256x256-2step.png` (132,762 B, 2026-11-17):
  covered by `.gitignore` (`/*.png`), not product files. They are the diagnostic 256 px / 2 step renders that the old README example produced.
* `D:\HCSInfer\image\`: an **empty** directory (0 entries, created 2026-11-17). It is not ignored; git simply does not see empty directories.
* `D:\HCSInfer\build\` holds ~60 build directories from earlier waves (large; ignored). `D:\HCSInfer\artifacts\` is ignored.

On drive C: free space at the start of this task was 27 GB (see `c_free_gb_before.txt`); nothing from this task was written there.
