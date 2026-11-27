# HCSInfer status

Last updated: 2026-11-27 (S0 in progress, release/v1.1-stable; earlier: 2026-11-26 wave A a4 baseline). Statuses: PASS / FAIL / BLOCKED / NOT RUN. Nothing below is claimed without an evidence file or an
inspected source file; items with no evidence are NOT RUN or BLOCKED.

## S0 (2026-11-27, in progress): verify the 41 unpushed commits before merging anything

Branch `release/v1.1-stable` = local `main` `1469638` + the S0 work; GitHub `origin/main` is still `0853143`.
Diff inventory: `docs/test-evidence/final-s0/repo-diff.txt` (41 commits, 365 files, +47,134/-427 lines). Plan and
branch/push policy: `docs/roadmap/PLAN_FINAL_STABLE.md`, `AGENTS.md` §27. Evidence: `docs/test-evidence/final-s0/`.

| Gate | Status | Evidence / reason |
|---|---|---|
| `scripts/build.ps1 windows-msvc-release` (clean configure, -j4, `/W4 /WX` on HCS code) | PASS | `build-cpu-verify.txt`: BUILD OK, exit 0; MSVC 19.44.35228; the upstream-only warnings (C4068, D9025 for `/EHc`, and the deliberately relaxed `/W3 /WX-` on the two httplib-facing files in `src/api/CMakeLists.txt`) are documented, not HCS warnings |
| `scripts/format.ps1` (clang-format 19.1.5) | PASS (red->green) | `format.txt` found `src/api/control_plane_image.cpp` unformatted; `-Fix` + recheck `format-GREEN.txt`: 298 files OK |
| `web/` `npm test` | PASS | `web-npm-test.txt`: 46/46 |
| Python `pytest` + `mypy --strict` | PARTIAL for this wave | `python-pytest.txt` 5 passed / 4 skipped (the real-EXE tests ran before the binary existed; they will be re-run), `python-mypy.txt` 0 issues in 8 files |
| `scripts/static-analysis.ps1 -Scope src -FailOnFindings` | **PASS** | red logs `clang-tidy.txt`, `clang-tidy-RED-ps51.txt`, `clang-tidy-RED-stderr.txt`, `clang-tidy-RED-findings.txt` (rounding in `models_remote_cmd.cpp`, three int→float narrowings in `tiling.cpp`, signed-char and divide-by-zero in `cpu_baseline.cpp`, a dead store in `merge.cpp`); each fixed at root cause; `clang-tidy-FINAL.txt`: **0 diagnostics in 92 TUs**, exit 0 |
| Full CPU CTest with real models (`HCS_TEST_*`, serial, lock) | **PASS** | `ctest-cpu.txt`: **1054 registered, 100% passed, 0 failed**, 9 skipped (4 Vulkan-only real tests, 3 ORT real-runtime tests without env, `e2e_gui_xss` Python 3.14 without `websockets`, and the agentic eval - see below); 1392.61 s. Real LFM3 text, SmolVLM2 vision, SD1.5 and FLUX.3 Klein renders all ran and passed. Free RAM logged before the run. |
| `e2e_llama_agentic_real` with `HCS_TEST_LFM3` set | **PASS** | `ctest-agentic-real.txt`: 1/1 in 179.84 s (the full CTest skip was only because this separate env var was missing; no defect) |
| Targeted CTest after the static-analysis fixes | **PASS** | `ctest-targeted-after-tidy-fixes.txt`: 133/133, incl. `CpuBaseline.GemmI8IsBitEqualToOracle` (the int64 conversion preserves bit-exact oracle agreement), all CLI, upscale and merge tests |
| Vulkan build | **PASS** | `build-vulkan.txt`: BUILD OK, exit 0, 876/876 steps; all 10 GLSL shaders compiled with `glslc` and validated with `spirv-val` (1.1/1.2 as declared) and embedded; `windows-msvc-release-vulkan` |
| Full Vulkan CTest with real models (`HCS_TEST_*`, serial, lock) | **PASS** | `ctest-vulkan.txt`: **1085 registered, 100% passed, 0 failed**, 4 skipped (3 ORT real-runtime tests without env, `e2e_gui_xss`); 1398.58 s. Real Vulkan text (`LlamaEngineReal.VulkanGeneration`, 10x load/unload, cancel, model switch), real Vulkan vision, real Vulkan SD1.5 + FLUX.3 renders, 32 `kernel-vulkan` tests and 10 `spirv-val` checks passed. Free RAM (10.87 GB) logged before the run. |
| S0 packaging dry-run | NOT RUN | queued after the STATUS update (S1/S4 will produce the release package) |

**Defects fixed in S0 so far (root cause, not assertion weakening):** (1) `scripts/common.ps1` printed the MSVC banner via
`cl 2>&1`, which PowerShell treats as a terminating `WriteError` under `$ErrorActionPreference='Stop'` - the build
aborted before configure (`build-cpu-RED.txt`); now the banner is read through `cmd.exe`. (2) `scripts/static-analysis.ps1`
piped `ConvertFrom-Json`'s array directly, which Windows PowerShell 5.1 passes as one `Object[]` - `Select-Object
-ExpandProperty file` failed; the parsed entries are materialised first. (3) The same script aborted on clang-tidy's
`N warnings generated.` stderr line under `-ErrorActionPreference='Stop'`; the native call now temporarily uses
`Continue` and still captures the full text for the findings filter.

## Wave B b3 (2026-11-26): image LoRA library, 4K upscaler pipeline, model merge (code side; no downloads)

Branch `wave-b/b3`, build `D:\HCSInfer\build\b3` (`windows-msvc-release`, CPU, `-j4`, `/W4 /WX`, clang-format 19.1.5 check OK).
Plan rows B3 (LoRA part), B4, D3. Evidence: `docs/test-evidence/wave-b/b3/README.md`. Docs: `docs/lora.md`, `docs/upscale.md`, `docs/merge.md`.

| Item | Status | Proven / not proven |
|---|---|---|
| LoRA manifest, scanner, compatibility rules, `loras` on API/MCP/CLI/UI, `GET /hcs/v1/loras`, `hcs.loras.list` | PASS (synthetic) | 11 unit + 13 contract tests (ids only, approved-root paths re-resolved at use, weight bounds, family mismatch = `lora_incompatible` before the render, missing library = `unsupported`); engine hand-off through `sd_opt_set_loras` compiles against the pinned header |
| Real render with a real LoRA | NOT RUN | no LoRA file exists locally and nothing is downloaded; behaviour of a LoRA on a quantised base is unmeasured |
| Upscale pipeline (tiling from the memory budget, feather blend, Lanczos to 3840, never beyond the cap), `upscale: off\|4k` on API/MCP/CLI/UI | PASS (test double + synthetic model) | 27 unit tests incl. NumPy golden for Lanczos; control-plane wiring with a nearest-neighbour test double; missing runtime/model = `unsupported` before the render; upscale failure keeps the render |
| ONNX Runtime backend, real `onnxruntime.dll` 1.31.1 CPU + synthetic ONNX model | PASS | `OrtRealRuntime.*` 3/3 with `HCS_TEST_ORT_DIR`/`HCS_TEST_ONNX_DIR` (skipped by default); DirectML EP NOT RUN (this ORT build has none) |
| Real super-resolution model (Real-ESRGAN etc.), time/RAM/PSNR/SSIM/LPIPS | NOT RUN | no model on this machine, no download |
| `HCSInfer merge` linear/SLERP/TIES/DARE/LoRA-into-base | PASS (unit + one real file) | 25 unit tests against NumPy goldens; CLI contract tests; real 320 MB `flux2-vae.safetensors` self-merge (linear and SLERP) bit-exact, 6 s, peak working set 46 MB. Self-merge is the identity, so merge quality is not proven; a merged model has NOT been loaded into an engine (NOT RUN) |
| Merge as HTTP/MCP job, GPU path | NOT IMPLEMENTED | CLI only (`docs/merge.md`) |

## Wave A baseline (a4, 2026-11-25/26): fresh build and full CTest of `main` `52ea9e7`

Fresh configure + build into `D:\HCSInferuild4-cpu` (`windows-msvc-release`) and `a4-vk` (`windows-msvc-release-vulkan`), `-j4`,
MSVC 19.44.35228, `/W4 /WX` clean on HCS targets. Full CTest run serially (`-j1`) with the real models (`HCS_TEST_GGUF`/`LFM3` = LFM3-Omni-2B Q8_0,
`HCS_TEST_GGUF2` = qwen3-0.6b Q4_K_M, `HCS_TEST_VLM`/`MMPROJ` = SmolVLM2-500M Q8_0, `HCS_TEST_DIFFUSION_MODEL` = SD1.5 Q8_0,
`HCS_TEST_FLUX3_MODEL` = FLUX.3 Klein UD-Q4_K_XL); real-model tests under `D:\HCSScratch
ealmodel.lock`. Evidence: `docs/test-evidence/wave-a/a4/`.

| Build | Registered | Passed | Failed | Skipped | Notes |
|---|---|---|---|---|---|
| CPU (`windows-msvc-release`) | 816 | 810 | 1 -> fixed | 5 | the failure was a test-harness defect (below); after the fix the test passed alone (`ctest-agentic-fix.txt`) = 811/816 passed effective, NOT a full re-run. Skipped: 4 Vulkan-only real tests (NOT RUN on a CPU build) and `e2e_gui_xss` |
| Vulkan (`windows-msvc-release-vulkan`) | 815 | 814 | 0 | 1 | skipped: `e2e_gui_xss`; the 4 Vulkan real tests PASS on the Radeon 680M; `e2e_llama_agentic_real` PASS with the fix |

* **Defect fixed (red -> green):** `e2e_llama_agentic_real` failed on a fresh checkout with `FileNotFoundError` because `tests/eval/agentic/e2e_lfm3.py` wrote its
  eval JSON into `artifacts/e2e-lfm3` before creating that directory (it only passed earlier because the directory already existed). Fix: create the evidence
  directory at start. Verified with the directory deleted beforehand on both builds. The eval itself reproduced the baseline 37/40 (tool use 0.906, coding 1.0, parallel 0/3).
* **Environment (NOT RUN):** `e2e_gui_xss` skips because the interpreter CMake selects (Python 3.14) has no `websockets` package; with another Python that has it the
  same script passes 8/8 (`xss-direct.txt`). No `out_of_memory` budget refusals occurred in these serial runs (7-10 GB free RAM before each phase).
* **Hosted CI (read-only `gh run list`, `github-ci-state.txt`):** `pr-quick-gate` success on `0853143`; `release` success on `40bf581` (draft pre-release, 8 assets);
  `hardware.yml` never completed (cancelled / pending). Tag `v1.0.0-rc.1` is pushed and an ancestor of `main`.
* Documentation audit: `docs/test-evidence/wave-a/a4/DOC_AUDIT.md` (stale CI / "never pushed" statements corrected in this file, README, packaging, release notes, CHANGELOG).
  Older statements further down that still say "never pushed" or "NOT RUN" for the hosted release workflow are superseded by this section.

## S1 (2026-11-27): new-feature correctness with real files

Evidence: `docs/test-evidence/final-s1/README.md`. Branch `release/v1.1-stable` at `daf4c3e` + this wave.

| Item | Status | Proven / not proven |
|---|---|---|
| Real image LoRA (LCM-LoRA SD1.5, id `lcm-sd15`) | **PASS** | CLI render with and without the LoRA at the same seed; JSON reports the applied id/weight; PSNR 11.50 dB / SSIM 0.64 prove a real change; both PNGs viewed and coherent. `lora-{baseline,real,compare}` |
| Real 4K ONNX upscale (Real-ESRGAN x4) | **PASS** | 512x512 -> 3840x3840, 4 tiles, 73.3 s, CPU provider explicitly reported (no DirectML in this ORT build), viewed image has no visible seams. `upscale-real.txt` |
| `HCSInfer tune --suite quick` on Radeon 680M | **PASS** | 10 results, all correct + 3 identical runs; 0 kernels default-enabled because the upstream GGML Vulkan op baseline is NOT RUN; noisy f32 GEMM bucket rejected. `tune-quick.json` |
| Merge of two distinct real models | **BLOCKED** | No same-architecture float pair locally; quantized SD1.5 + SD-Turbo gives a clean `invalid_argument` (1131 vs 1323 tensors), quantized self-merge gives `unsupported` (exit 10). Downloads require owner approval. |
| LoRA/upscale/merge/admission/vault/CLI/planner/image-profile/guard tests | **PASS** | covered by the S0 CPU (1054/1054) and Vulkan (1085/1085) CTest runs; new features are not mocks in those suites where a real file was available |
| Real trainer run, real `prime-agent` session, real WebGPU | NOT RUN | external model/credential/hardware dependencies |

## S2 (2026-11-27): real-model E2E and controlled benchmarks

Evidence: `docs/test-evidence/final-s2/README.md`. Branch `release/v1.1-stable` at `94d879d` + this wave.

| Item | Status | Proven / not proven |
|---|---|---|
| B0 CPU / B1 Vulkan full benchmarks, same LFM3 hash/seed | **PASS** | CPU 18.97 t/s decode, 2579 ms TTFT; Vulkan 35.97 t/s, 311.9 ms TTFT; **1.90x decode / 8.27x TTFT** on the 680M; raw samples, p90 and all metadata in `bench-{cpu,vulkan}-full.json`. One machine, no thermal data, page cache not flushed. |
| HCS Studio on the Vulkan build with real models | **PASS** | `studio-shots-vk.txt` 42/42: real model load, streamed chat, Stop/cancel, t2i + i2i, no CSP/console errors; `serve-vulkan.log` shows the real 202/200 responses. Wave C's "Vulkan-engine chat in the UI" gap is closed. |
| HTTP/MCP real inference and agentic eval | **PASS** | S0 CPU and Vulkan CTest e2e suites (see S0 section); `e2e_llama_agentic_real` PASS with `HCS_TEST_LFM3` on both builds. |
| Benchmark provenance | caveat | the EXEs were configured at `6bfe340+dirty`; S4 rebuilds static EXEs from the final clean tree and records hashes. |

## S3 (2026-11-27): hardening / soak

Evidence: `docs/test-evidence/final-s3/README.md` (branch `release/v1.1-stable`, commit `35c3bd6` + this wave).

| Item | Status | Evidence / reason |
|---|---|---|
| Full soak profile (100 load/unload cycles, 30 min generation, vision/image repeats, cancel storm, shutdown, unplug fault injection, HTTP storm) | **PASS** | `soak-full/summary.json`: **13/13 phases PASS**, exit 0; 100 cycles 118.8 s, 30 vision runs, 20 diffusion runs, 60 cancel rounds, 3 shutdown phases, 30-minute generation (1807.3 s), 100 unplug-injection rounds, HttpStorm 63.1 s. Free RAM 11.54 GB logged before the run. |
| Leak / resource growth | **PASS** | `soak-core.jsonl`: load/unload private growth 41.0 MiB (limit 128), handle growth 0; 30-min generation private growth 5.68 MiB, handle growth -1; all verdicts PASS. |
| Throughput drift over 30 minutes | **PASS** | 256 samples, first fifth 18.97 t/s vs last fifth 19.09 t/s (**+0.63%**). |
| HttpStorm | **PASS** | `http-storm.json`: 18/18 checks (FIN/RST storm, abandoned requests, 64 sockets, unload/reload, kill+restart, Ctrl+Break). |
| Parser fuzz smoke (UBSan, no ASan) | **PASS (smoke)** | `libfuzzer-smoke.txt`: 376,006 executions in 61 s from an empty corpus, 0 crashes / 0 UBSan reports, peak RSS 30 MB. ASan unavailable with this toolchain; long campaign NOT RUN. |
| Runtime XSS regression | **PASS** | `xss/xss-run.txt`: 9/9 checks; previously skipped because `websockets` was missing. |
| Physical USB pull, true RAM exhaustion, Vulkan soak, long fuzz, stress harness abort re-run | NOT RUN | documented fault injection/substitutes only |

## S4 (2026-11-27, in progress): release package and docs

Branch `release/v1.1-stable` at `d185678` + this wave. Version bumped to **1.1.0-rc.1** (CMake, wheel, web package,
README, install/security/support docs, CHANGELOG, release notes). Evidence: `docs/test-evidence/final-s4/`.

| Item | Status | Evidence / reason |
|---|---|---|
| Python build venv (`build` + `twine`) | PASS | created under `build/devtools/pybuild` (D:), `build+twine OK` |
| Static CPU `/MT` EXE from a clean tree | **PASS** | `build-static-cpu.txt`: BUILD OK, 515/515 steps; `HCSInfer.exe --version` = **HCSInfer 1.1.0-rc.1** |
| Static Vulkan `/MT` EXE | **PASS** | `build-static-vulkan.txt`: BUILD OK, 876/876 steps, 10 shaders compiled/validated/embedded |
| Restricted-env proxy (both EXEs, real model) | **PASS** | `restricted-static-{cpu,vulkan}.json`: empty PATH, help/doctor/real generation/serve+GUI 200, 0 modules outside `C:\Windows` (Vulkan adds `vulkan-1.dll`); fixed a real PS 5.1 `Kill(bool)` defect red->green. A clean-machine run remains NOT RUN. |
| Portable ZIP / wheel / sdist / web dist / SBOM / SHA-256 | **PASS** | `release-local.txt`: CPU EXE `e1daef64...`, Vulkan EXE `885c79d7...`, ZIP `77974fa5...`, wheel + sdist (`twine check` PASS), SBOM 15 components; `release-verify.txt`: 7/7 hashes, LF endings |
| Web tests / wheel smoke | **PASS** | `web-npm-test.txt` 46/46; `wheel-smoke.txt`: fresh venv, `HCSInfer 1.1.0-rc.1` |
| Licences | PARTIAL | prime-agent submodule not initialised -> its LICENSE file is missing from the package; reported by the script |
| Docs reconciled with the binary | **PASS** | README/CHANGELOG/install/security/support/benchmarks/release notes and LoRA/Upscale/Merge docs updated in this wave; final gate in S5 |

## S5 (2026-11-27, in progress): final gate, merge and tag

Evidence: `docs/test-evidence/final-s5/`. Working tree clean at `3c7680f` before the final builds.

| Item | Status | Evidence / reason |
|---|---|---|
| Fresh static CPU + Vulkan builds from the final commit | **PASS** | `build-static-cpu.txt` (515/515 steps) and `build-static-vulkan.txt` (876/876, 10 shaders validated/embedded); after a reconfigure (`reconfigure-static-*.txt`) both EXEs embed the final clean tree and print `HCSInfer 1.1.0-rc.1` |
| Full CPU CTest on the static preset with real models | **PASS** | `ctest-static-cpu.txt`: **1054/1054, 0 failed**, 8 skips (Vulkan-only/ORT/XSS-with-CMake-Python; XSS separately PASS in S3); 1555.77 s; free RAM logged |
| Full Vulkan CTest on the static preset with real models | **PASS (2 of 3 runs green; 1 transient failure)** | Run 1 `ctest-static-vulkan.txt`: 1084/1085, `VulkanRunner.DispatchTimeoutMarksTheRunnerLostAndResetRecovers` failed at `prepare_gemm_f32` (`op.ok()` false). Repro attempts: isolated test SKIP (NOT RUN), 32-test kernel group 32/32, range 1000–1025 26/26. After adding the error message to the assertion (diagnostic only), run 2 `ctest-static-vulkan-rerun.txt` and run 3 `ctest-static-vulkan-run3.txt` are **1085/1085, 0 failed**. Root cause not reproduced; classified as transient device/driver state, recorded as a residual risk, not hidden. |
| Format / clang-tidy / web | **PASS** | `format.txt` 298 files, `clang-tidy.txt` 0 diagnostics in 92 TUs, `web-npm-test.txt` 46/46 |
| Wheel / SHA-256 / restricted-env final re-check | **PASS** | `release-local-final.txt`: package rebuilt from commit `e81d002` (CPU EXE `e440172c...`, Vulkan EXE `b639bd8e...`, ZIP `c156cf85...`), `release-verify-final.txt` 7/7 hashes, `wheel-smoke-final.txt` = 1.1.0-rc.1, `restricted-final-{cpu,vulkan}.json` 0 modules outside Windows, health/GUI 200 |
| Merge `release/v1.1-stable` into `main`, push, tag `v1.1.0-rc.1` | **PASS** | merge commit `e958241` on `main`, pushed to `origin/main`; annotated tag `v1.1.0-rc.1` pushed; follow-up commit `24383ed` records it. GitHub **draft pre-release** created with 8 verified assets (`gh release view v1.1.0-rc.1`); publishing/signing remains the owner's step. No force-push. |
| Hosted `pr-quick-gate` on `main` | **PASS (after 3 red→green iterations, post-tag)** | runs `45943833293` (3 `MergeFiles.*`) and `45947626112` (`CliMerge.LinearMergeWrites...`) failed because hosted `TEMP` is on `C:` and the merge engine correctly refuses C: outputs; `45951078677` then failed only on clang-format. Shared `test::safe_temp_base()` used by both fixtures (`tests/support/temp_dir.hpp`, `tests/unit/merge/test_merge_files.cpp`, `tests/contract/cli/test_cli_merge.cpp`), helper reformatted; repro with `TEMP=C:\...\Temp` locally: **18/18 PASS** on both static presets. Final run **45953513669 = success** on commit `02067f0` (all six jobs). Test/format-only fixes; shipped EXEs unchanged, tag stays at `e958241` (no force-push). |

### Engineering report (S5, final values completed below)

```text
HCSInfer engineering report
- Current phase: S5 final gate (release/v1.1-stable), version 1.1.0-rc.1
- Commit: e81d002 (final builds reconfigured from a clean tree), merge commit e958241 on main, tag v1.1.0-rc.1
- Build status: PASS - static CPU 515/515 steps, static Vulkan 876/876 steps, 10 shaders validated
- Tests: CPU static CTest 1054/1054 PASS; Vulkan static CTest 1085/1085 PASS on 2 consecutive final runs (1 earlier transient failure of the dispatch-timeout test, reproduced green in isolation and in 2 full reruns; see S3/S5 evidence)
- Real Radeon 680M test: PASS (S0 Vulkan CTest 1085/1085, S2 Studio 42/42, real tune)
- Real text / VLM / diffusion model: PASS (S0 + S5 CTest with HCS_TEST_* set)
- API/MCP: PASS (experimental, scripted stdio client; Streamable HTTP intentionally out of scope)
- Storage/HDD: PARTIAL (vault prefetch measured, Stage B BLOCKED, physical unplug NOT RUN)
- Security/licence: PARTIAL (injected fault tests and audits PASS; signing, cargo deny, clean VM NOT RUN)
- Performance evidence: docs/test-evidence/final-s2 (B0/B1 controlled, 1.90x decode / 8.27x TTFT)
- Known limitations: docs/release-notes-1.1.0-rc.1.md
- Remaining blockers: owner publish/sign/clean-machine/Vercel; real merge pair, real trainer/agent, ASan/long fuzz
- Exact next action: merge to main, push, tag v1.1.0-rc.1, prepare the draft release
```

## Audit fixes (2026-11-21, after wave R)

A release audit found overstated or stale claims in README/ADR-008/STATUS/benchmarks (single-file wording, benchmark binary
provenance, TTFT ratio, stale wave-B text and limitations, CI badge, CLI help, ZIP links, CRLF checksums, LFM3 byte size, missing
runtime XSS test). All fixed at the root and re-verified; the version stays **1.0.0-rc.1**. Binaries were rebuilt from clean commit
`ae3bbbd` (embedded `ae3bbbdc4050`; later commits are documentation/evidence/packaging-script only): CTest on
`windows-msvc-release-static` 812 passed + 4 skipped (816 registered; first run 6 `out_of_memory` budget refusals under `-j2`, re-run
serially 7/7 PASS) and on `windows-msvc-release-static-vulkan` 815 passed (815 registered; first run 2 budget refusals, re-run 2/2
PASS); clang-format/clang-tidy/`npm test`/link check green; the shipped EXEs passed an empty-PATH run and a quick benchmark. The
earlier "815 passed" wording for the CPU run was wrong: 811 passed + 4 skipped. Index: `docs/test-evidence/release-final/README.md`.
(Correction, wave A a4: this was later pushed; see the Wave A baseline section for the hosted CI state.)

## Repository state

Wave B (distribution, 2026-11-20, branch `claude/repo-analysis-v1-stable-d7c553`; since merged into `main` and tagged `v1.0.0-rc.1`, see the Wave A baseline section): two static EXEs (CPU `HCSInfer.exe` with system-DLL imports only, `HCSInfer-vulkan.exe` which also imports the system
`vulkan-1.dll`; neither is called "single-file" until the clean-machine test, NOT RUN), Python wheel/sdist `hcsinfer`,
`release-local.ps1`, `release.yml`, web dist, `docs/install.md`, `docs/packaging.md`; evidence `docs/test-evidence/wave-b/`.
Still open (owner or environment dependent): clean-machine test, signing (the hosted `release.yml` run is done: green on `40bf581`, draft pre-release). CTest on the static
presets was run in the final audit (see `docs/test-evidence/release-final/`).

Wave D (benchmark data from the wave-B static EXEs, 2026-11-20; these are NOT the shipped release binaries, which were rebuilt
later - see the audit-fix section of `docs/benchmarks.md` and `docs/test-evidence/release-final/`): **PASS (records), partial coverage.** The static CPU and Vulkan EXEs of wave B ran
`benchmark --suite full` (5 iterations, medians/p90) for qwen3-0.6b Q4_K_M and LFM3-Omni-2B Q8_0, a `llama-bench` cross-check, KV cache
types F16/Q8_0/Q4_0 at 4K/8K (llama-bench, the HCS engine has no cache-type option), real vision latency, SD1.5 and FLUX.3 Klein 512x512
renders (FLUX: N=1 per cell, SD1.5 CPU N=2), HTTP streaming TTFT and read-only drive reads (38-63 MiB/s cold on the USB disk). Findings:
Vulkan ~2x decode; warm TTFT 2.8x (qwen3-0.6b) to 7.8x (LFM3-Omni-2B), vision 7.3x; FLUX.3 is not faster on Vulkan here (566 s vs 508 s fast; unexplained vs wave F's 364-471 s);
quantised KV is 4-5x slower on CPU. NOT RUN: thermals, flushed-cache cold start, image/KV-quality metrics. Evidence
`docs/test-evidence/benchmarks/`, report `docs/benchmarks.md`, tooling `tools/bench/` (driver, report/SVG generator, `regression-thresholds.json`
with the >5% rule, `check_regression.py`).

- Public remote `https://github.com/timfromhcs/HCSInfer`. `main` has the V1-stable waves and the M8 kernel:
  `902b41e` (W1 `POST /hcs/v1/benchmarks` + `cache` + `benchmark --suite full`), `5633450` (W2 agent/train salvage),
  `b02075f` (W3 soak defects + test gaps), `af884c3` (MCP `hcs.runtime.benchmark`), `7287554` (W4 static-CRT
  experiment), `dd9be7a` (D-S5 `read_file_limited`), `f9d03a5` (D-S6 benchmark prefill), `926c47c` (M8 Q8_0 GEMV
  kernel) and the docs/evidence commits after them. The history starts at `d5baf76` "Initial commit" (2026-11-17).
- CI (superseded, see the Wave A baseline section: `pr-quick-gate` is green on `0853143` and the hosted `release` run is green on `40bf581`). The runs below are for older commits on `main`.
  `.github/workflows/pr.yml` run **44621025517** (commit `7b393eb`) is green on all six jobs (2026-11-19),
  including the Release leg's package step (portable folder + ZIP + SBOM + `SHA256SUMS.txt` verified and uploaded as
  artifacts) and `clang-tidy` under the changed runner (VS 18 / LLVM 22; see the clang-format/clang-tidy row). It
  follows green runs 44607088082 (`427d70e`), 44598980056 (`6010bb2`), 44592044647 (`0bff2ef`) and a red-then-fixed run
  44272921951 (`af884c3`, D-S5). Model-dependent tests skip on the hosted runner (NOT RUN), so CI proves
  configure/build/unit/contract/parser and packaging on a clean hosted machine, not Vulkan or real-model behaviour; the
  M8 kernel is built and tested locally (the preset is off in CI). `hardware.yml` (self-hosted Radeon runner) has never
  completed a run (wave A a4 `gh run list`: runs cancelled, one pending).
- Branches: `main` and the docs worktree branch `claude/repo-analysis-v1-stable-80f8d6` (== `main`). The local branch
  `claude/flux3-agent-engine-v1-665ab0` is now an **ancestor of `main`**; its unique work was salvaged selectively and
  committed in W2 (`5633450`), with the web/ and CMake differences deliberately not taken.
- Wave C (2026-11-20, HCS Studio UI/UX polish): safe DOM-only Markdown for chat/vision replies, copy buttons and toast, first-run guide, Performance Center with measured tokens/s + memory split + device cards, Settings reset, `role="log"`/`aria-busy` streaming. Real headless Edge on `HCSInfer serve` (CPU build, real LFM3-Omni-2B Q8_0 and SD1.5 Q8_0): `studio_shots.py` 42/42 checks (every tab, no horizontal scroll at 1360/820/390, light+dark shots, chat load/stream/Stop, txt2img + img2img, 0 CSP violations, 0 console errors) and the new `studio_md.py` 16/16 (Markdown -> elements, `<img onerror>`/`<script>` from the model shown as text, tokens/s after chat, manual theme, reset). Full CTest (Release, CPU, `HCS_TEST_GGUF` set): 815 registered, 23 skipped (NOT RUN: need other models or a GPU), 0 failed; `web/` `npm test` 46/46. Evidence: `docs/test-evidence/wave-c/`; screenshots: `docs/screenshots/`. NOT RUN / limits: the clipboard write itself (headless Edge refused it; the fallback toast was verified, a real click in a normal browser was not), Playwright-driven `e2e_gui_real` interaction phase (Playwright not installed; the equivalent CDP scripts above ran), screen-reader testing, Vulkan-engine chat in the UI, the `web/` demo UI was not restyled.
- Wave G (2026-11-20): cpp-httplib pin moved v0.37.2 -> **v0.44.0** (`811dd0b6...`) after an OSV query listed 9 advisories (4 server-side) for the old pin; `external/prime-agent` is now initialised in this worktree at its recorded pin.
- Submodule pins: `external/llama.cpp` d3c3e242c16b8cbf51400fad410cd69b22d64f35, `external/stable-diffusion.cpp`
  532d2c15c430a785a712b2d94281f04e733008a4, `external/prime-agent` 8980e66f2f07762d62a6c1d73b347a4cac5275cc
  (see `external/dependency-lock.json`).
- Toolchain on the target machine: MSVC 19.44.35228 (toolset 14.44.35207) x64, CMake 4.4.0, Ninja 1.13.2,
  Vulkan SDK 1.4.357.0 (build time only), Windows 11 build 10.0.26200.9974, Ryzen 7 7735HS, Radeon 680M
  (device id 0x1681), ~21.2 GB RAM, AMD driver 32.0.21043.12001.
- Modules present in `src/`: `core`, `hardware`, `storage`, `kernels/cpu`, `kernels/vulkan` (the latter only with
  `-DHCS_ENABLE_VULKAN_KERNELS=ON`), `agent`, `train`, `process`, `engines/llama`, `engines/diffusion`, `gui`, `api`,
  `mcp`, `cli`, `app`. **Still absent:** `memory`, `scheduler`, `telemetry`. `shaders/` now exists
  (`shaders/llm/gemv_q8_0.comp`). A separate browser deliverable `web/` (HCS Studio Web, ADR-013) exists outside
  `src/` and outside the CMake graph.
- The two smoke-test PNGs at the repository root (`cpu-256x256-2step.png`, `cpu-edit-256x256-2step.png`) are
  ignored by `.gitignore` (`/*.png`) and are not part of the product.

## Gate table

| Gate | Status | Evidence / reason |
|---|---|---|
| Full Release CTest suite, **with the real models** (`HCS_TEST_*` set), CPU build and Vulkan build | **PASS** | **wave R final (2026-11-20, 1.0.0-rc.1, HEAD of this branch): CPU build `windows-msvc-release`: 815 registered, 811 passed, 0 failed, 4 skipped (the four Vulkan-only real tests, NOT RUN on a CPU build) - `docs/test-evidence/release-final/ctest-release-cpu.txt` (1226 s, includes the real LFM3-Omni-2B / SmolVLM2 / SD1.5 / FLUX.3 tests and the e2e scripts). Vulkan build `windows-msvc-release-vulkan`: 814 registered, 809 passed on the first run and 5 failed - all five (`ShutdownReal.*` x2, `e2e_api_mcp_real`, `e2e_llama_agentic_real`, `e2e_vision_real`) were refused by the memory budget with `out_of_memory` because only ~0.7-1.0 GB RAM was free during that run (other processes); with ~9 GB free the same five re-ran 6/6 PASS (`ctest-release-vulkan-rerun-failed5.txt`). The first-run log is kept unedited: `ctest-release-vulkan.txt`. No source change was needed (the budget refusing a load that does not fit is the intended behaviour).** `web/` `npm test` 46/46 - `release-final/web-npm-test.txt`. Earlier runs: wave R RC (814 registered, 778 passed, 36 skipped with no models set - `wave-r/ctest-release-cpu.txt`; `kernel-cpu` + `kernel-vulkan` 12/12 on the real 680M - `wave-r/ctest-kernels.txt`) and wave G (`wave-g/ctest-release-cpu-httplib-0.44.txt`). |
| Configure + build Debug / Release (MSVC x64, Ninja, `/W4 /WX` on HCS targets) | PASS | `M2-core/`, `M3-api-mcp/ctest-debug.txt`; the Vulkan preset configures and builds (`M2/image/README.md`) |
| M0 hardware doctor (CPU features, RAM, OS, Vulkan devices/heaps, GPU adapters, drive info, read bench) | PASS | `M0/` (`doctor.json`, `doctor-no-vulkan.json`, `readbench-usb-hdd.json`, `vulkaninfo-summary.txt`) |
| M1 llama.cpp independent baseline, CPU + Vulkan, real GGUF from USB drive | PASS | `M1/llama/README.md`; qwen3-0.6b Q4_K_M, sha256 `4ea79412...` |
| M1 stable-diffusion.cpp baseline, patched vs `SD_USE_UPSTREAM_GGML=ON`, CPU + Vulkan | PASS for the standalone SD1.5 Q8_0 baseline (FLUX.3 Klein is proven later, through the HCS adapter, in wave F; it was not run through the standalone upstream CLI) | `M1/sd/README.md`; 512x512 output byte-identical between GGML modes once; model licence unverified |
| B7 compute backend vs parameter backend (sdcpp `--backend` / `--params-backend`) | PASS (measured, one sample each) | `B7-params/`: vulkan0+cpu-params 5.22 s / 1773 MB peak WS, vulkan0+disk-params 3.29 s / 625 MB, cpu+cpu 9.68 s; outputs differ between placements (equivalence NOT verified); the model is on internal NVMe, so a USB-HDD disk-params case is NOT RUN |
| M1 multimodal (libmtmd) upstream proof | PARTIAL: NOT RUN as a standalone `llama-mtmd-cli` run; the pinned libmtmd is proven through the HCS adapter | `M2-vision/` (SmolVLM2-500M Q8_0 + mmproj, CPU and Vulkan) |
| M2 headless core (lifecycle, registry, JobQueue, MemoryBudget, PathGuard, settings) | PASS | unit tests; `M2-core/` |
| M2 control-plane facade: text or image models, switching, freeing the previous model | PASS (text and image, real) | `M2/llama-adapter-README.md`, `M2/image/README.md`, `M3-image/` |
| M2 llama adapter (text), CPU + Vulkan, cancellation, 10x load/unload | PASS | `M2/llama-engine-integration.txt` (16/16) |
| M2 llama adapter vision (`analyze_image`) | PASS (SmolVLM2-500M Q8_0 only; CPU and Vulkan) | `M2-vision/README.md`: 10/10 real-model engine tests on the Vulkan build including bad image / mismatched mmproj / cancel / reload |
| M2 diffusion adapter text-to-image + img2img, CPU + Vulkan | PASS | `M2/image/README.md`; integration tests `DiffusionBlocking.*` (6/6) now run with `HCS_TEST_DIFFUSION_MODEL` |
| CLI | PASS for `doctor`, `models scan/list/inspect`, `chat`, `vision`, `image generate/edit`, `serve`, `mcp stdio`, `benchmark --suite quick|full`, `cache stats|clear`, `train validate/gate/adapter/pipeline` | `tests/contract/cli`, `M2-vision/`, `benchmark/`, `docs/test-evidence/train/cli-demo.txt`. **`models load`, `models unload` and `tune` return exit code 10 (Planned)** (`src/cli/cli.cpp`); model load/unload works over HTTP and MCP |
| `HCSInfer benchmark --suite quick` (real text benchmark, AGENTS.md 21.1 JSON) | PASS (one real CPU run) | `benchmark/`: B0-text, CPU, qwen3-0.6b Q4_K_M, cold+warm phases, full metadata, sha256 recorded. `--suite full` runs on a real model too (LFM3-Omni-2B Q8_0, CPU, sha256 `59118f66...`): prefill + decode sweeps all PASS - `benchmark/full-suite-real.json` |
| M3 HTTP API: `/v1/models`, `/v1/chat/completions` (+streaming, OpenAI `tools` / `tool_calls`, `tool_choice` auto or none), `/hcs/v1/{health,runtime,devices,memory,models,jobs}`, auth, loopback, limits | PASS (experimental) | `tests/contract/api*`, `M3-api-mcp/`, `M10-fixes/e2e-api-mcp-real-after-fix.json`; tool calling: `lfm3/` (19 unit + 12 contract tests, real model in `e2e-lfm3-*.json`; `tool_choice` required / named function and legacy `functions` answer 501) |
| M3 HTTP image routes `/v1/images/generations`, `/v1/images/edits` | **PASS (experimental)** | `M3-image/`: 47/47 checks over a real socket with SD1.5 Q8_0 on the **CPU build and the Vulkan build** (`vulkan-e2e-image.json`, plus the four `vulkan-*.png` renders) - real PNG of the requested size (not blank), edit keeps the input size, validation/limits, `multipart` -> 415, abandoned request cancels the render |
| HTTP vision input on `/v1/chat/completions` (`image_url` data URI, streaming, cancel) | PASS (experimental; SmolVLM2 only) | `M2-vision/` (`e2e_vision.py` 27/27), `tests/contract/api_vision`, `e2e_vision_real` |
| M3 MCP stdio (models, chat, runtime, jobs, benchmark tools; path guard) | PASS (experimental) | `tests/contract/mcp`, `M10-fixes/e2e-api-mcp-real-after-fix.json`; `hcs.runtime.benchmark` runs the shared benchmark runner as a job - `docs/test-evidence/mcp-benchmark/` (real LFM3-Omni-2B Q8_0 over stdio, AGENTS.md 21.1 document) |
| MCP `hcs.vision.analyze` | PASS (experimental; real VLM over stdio) | `M2-vision/`, `tests/contract/api_vision` |
| MCP `hcs.image.generate`, `hcs.image.edit` | **PASS (experimental)** | `M3-image/`: real render, image returned as an MCP image content block, size kept, approved-root enforcement, cancellation |
| `POST /hcs/v1/benchmarks` | PASS (contract tests, no model) | `202` + job, result via `GET /hcs/v1/jobs/{id}`, cancel, `404`/`400`/`501`, body <= 4 KiB (`tests/contract/api/test_http_benchmarks.cpp`); real-model run NOT RUN |
| MCP `hcs.runtime.benchmark` | **PASS (experimental; real-model run recorded)** | `tests/contract/mcp` (`McpTools.Benchmark*`); real LFM3-Omni-2B Q8_0 over stdio: `PASS`, CPU, cold load 4404 ms, warm TTFT 2574 ms, 18.85 decode t/s, peak RAM 2.42 GiB - `docs/test-evidence/mcp-benchmark/` |
| MCP Streamable HTTP | NOT RUN (intentionally not implemented) | `docs/mcp.md` |
| M4 GUI (embedded local web UI, ADR-009 accepted) | **PASS (experimental)** for the HTTP/browser-boot phases, on both builds | `M4-gui/`: 23 checks 0 failed over a real socket (CSP, traversal, Host/Origin, 405, token not embedded) **plus a real headless-Chrome boot** that connects with the fragment token and lists the models; the same script against the **Vulkan build** is `vulkan-e2e-gui.json` (22 checks, 0 failed - one check needs a registered model). Python-`playwright` interaction phase NOT RUN (playwright not installed) |
| M4 normal-user workflow without a terminal | PARTIAL | manual run recorded in `M4-gui/README.md` (load, streamed chat, Stop/cancel, vision, Performance); not automated |
| V1.1 wave 1: LFM3-Omni tool calls + agentic eval, FLUX.3 Klein plumbing + presets, HCS Studio UI, agent/train foundations | PASS (experimental) | `docs/test-evidence/lfm3`, `flux3`, `agent`, `studio`; the FLUX.3 render was BLOCKED then; unblocked by wave F. Commit `6a390b8` (V1.1 wave 1) |
| **Wave F** FLUX.3 Klein render | **PASS** (Vulkan t2i + i2i, CPU via test; single-sample timings) | Real 512x512 renders with Qwen3-VL-4B Q4_K_M + FLUX.2 VAE (hashes in `docs/test-evidence/wave-f`), 364-471 s on Radeon 680M, sweep 4/6/8/12/16 with PSNR/DSSIM, `fast` preset 8 -> 6 steps, engine default cfg for FLUX.3 fixed to 1.0 (default cfg over-saturated). `Flux3PresetsReplaceStepsAndReportThem` now runs (`hcs_test_diffusion_engine` 14/14). Full ctest on the Vulkan build: 788 tests, 1 failed (`e2e_gui_real`, headless-browser phase/Playwright missing, environment), env-gated llama/vision real tests skipped. LPIPS, repeats, `params-backend disk` NOT RUN. `docs/test-evidence/wave-f/README.md` |
| **Wave G** hardening gaps (OOM injection, USB-disconnect, shutdown, supply chain, restricted-env EXE) | **PASS** for items a-d (red->green where a defect existed); **PARTIAL** for e (see the one-EXE row) | `docs/test-evidence/wave-g/README.md`. (a) `MemoryBudget` fault-injection seam + `error_from_engine_exception`: **defect fixed** - `std::bad_alloc` escaping an engine was a generic `BackendError`, now `OutOfMemory` (red `oom-red-before-fix.txt` -> green), 8 tests. (b) 6 `Unplug.*` tests (source vanishes at every chunk boundary, invalid handle, handle release). (c) 7 fake-engine + 3 real-model (LFM3-Omni-2B Q8_0, CPU) shutdown tests, all PASS. (d) `npm audit` 0 / `cargo audit` + licence check of prime-agent / OSV query of the native pins: **cpp-httplib v0.37.2 had 9 advisories (4 server-side) -> pin bumped to v0.44.0**. Also fixed a harness defect: `e2e_gui.py` picked a non-functional Edge before the working Chrome |
| V1.1 wave 2: HCS Agent foundations (policy, loopback provider document, launch plan, Windows process supervisor, ADR-011) | PASS (logic + supervision) / real agent run NOT RUN | `hcs_agent` gained `agent_config` (loopback-only provider document + a launch plan scoped to an approved root, `--offline`) and a new `hcs_process` module (spawn without a shell, bounded merged-output capture, timeout, whole-tree kill via a kill-on-close Job Object). 8 `ChildProcess.*` tests run against a real helper executable, including a grandchild proven dead after the job is closed. Full suite **518/518**; clang-tidy clean over 57 TUs. **NOT RUN**: a real `prime-agent` session and the venv bootstrap. `docs/agent.md`, `docs/test-evidence/agent/` |
| V1.1 wave 2: HCS Train pipeline (evaluation-report ingestion, eval gate, adapter book, `HCSInfer train` CLI, ADR-012) | PASS (logic + CLI) / real training NOT RUN | `hcs_train` gained `eval_report`, `adapter_book`, `pipeline` and the `train` CLI. 25 unit + 5 CLI contract tests; full Release suite **505/505, 0 failed**; clang-tidy clean over 55 TUs; clang-format clean over 164 files. Promotion is refused unless the gate kept the adapter (exit 11); reports and manifests are parsed fail-closed and bounded; ids cannot escape the book. No trainer run and no real adapter exist. `docs/train.md`, `docs/test-evidence/train/` (`cli-demo.txt`) |
| W2 port of the stopped `flux3-agent-engine` session: agent redaction/event parser/tool audit/session manager, LFM3 chat template + dataset prep, RSI loop (uncommitted) | PASS (logic + process supervision against a stand-in agent) / real `prime-agent` session NOT RUN / real trainer NOT RUN / Jinja parity NOT RUN | +185 tests (550 -> 735, 0 failed). `ToolAudit` is a tripwire, not a sandbox (`docs/agent.md`, documented limits are tested as limits). `hcs::process` extended (streaming, replaced environment, Job Object process/memory caps) with one launcher seam; the session manager is exercised with a scripted fake launcher and with the real Job-Object launcher driving `hcs_child_probe` (token only in the child environment, grandchild killed on cancel, flood/oversized-line caps). `rsi_loop` reuses `EvalReport`/`evaluate_gate`/`run_train_pipeline`/`AdapterBook` (no duplicate types). `tools/train/jinja_reference.py` was not run (`jinja2`/`gguf` not installed). Not wired into HTTP/MCP/UI. `docs/agent.md`, `docs/train.md` |
| V1.1 wave 2: HCS Studio Web (`web/`, browser WebGPU/WASM, ADR-013) | PASS for the shell + logic / real WebGPU inference NOT RUN | `web/`: 46 unit + static security tests (`npm test`, 46/46) and `tests/e2e/boot.mjs` on a real static server + real headless Chrome (14/14, `docs/test-evidence/web/web-boot.{json,txt}` + `web-chat.png`); the headless browser has no WebGPU adapter so it fell back to the WASM path. CI run 44167798984 is green on all six jobs; the headless step found that a browser can expose `navigator.gpu` while `requestAdapter()` never settles, so the probe now times out (4 s) and falls back instead of hanging. Both web steps are blocking gates. Image Studio/Train/Agent are "Local only"; Vercel deploy NOT RUN (needs owner credentials) |
| M5 Model Vault v1 | PARTIAL | Stage A (upstream load modes) **measured**: all six llama.cpp modes work; `direct_io` costs ~14x load time on the USB HDD because it bypasses the file cache, mmap-backed modes commit ~250 MB less than read-into-RAM, no throughput difference while the model fits in RAM - `M5-loadmodes/`. Stage B (bounded async pipeline feeding the engine) **BLOCKED**: no model larger than the 21 GB RAM is available locally and the OS file cache cannot be flushed without administrator rights, so a controlled storage-bound comparison cannot be produced on this machine; `src/storage` reads are tested read-only on a real GGUF, engines do not load through the vault. USB-unplug test NOT RUN |
| M6 HCSMODEL container (header, chunk table, Raw/LZ4/Zstd, hashes, random access) | PASS (experimental) / PARTIAL for fuzz | 59 unit + 3 parser + 2 integration tests; libFuzzer smoke only 60 s with UBSan, no ASan (`M6-storage/`). Throughput benefit on a real HDD NOT proven |
| M7 shared GGML ABI (llama.cpp GGML + sd.cpp via `SD_USE_UPSTREAM_GGML=ON` in one binary) | PASS for build + SD1.5 + llama text in one binary | `HCSInfer.exe` links both; coverage for Flux/DiT/Qwen-Image, FP8/INT8 NOT RUN |
| M7 single `HCSInfer.exe`, static CRT, clean-machine test | **PARTIAL** (wave B: static CPU `HCSInfer.exe` and Vulkan `HCSInfer-vulkan.exe` built and proven in a restricted environment; clean-machine NOT RUN) | Wave B (2026-11-20): preset `windows-msvc-release-static-vulkan` (/MT + Vulkan) builds without CRT conflicts (666 steps). `dumpbin`: CPU EXE (44,229,120 B, sha256 `c3a613e6...`) imports ADVAPI32/WS2_32/KERNEL32 only; Vulkan EXE (86,548,992 B, `b94a6e06...`) adds `vulkan-1.dll`, so two files ship (a loader-importing program cannot start without a GPU driver; delay-load not attempted). `scripts/restricted-env-check.ps1` (empty dir, empty PATH): help, doctor, real LFM3-Omni-2B generation (CPU; Radeon 680M for the Vulkan EXE), `serve` health 200, GUI page 200, all loaded modules under the Windows dir. Clean-machine test NOT RUN (Windows Sandbox absent, feature query needs elevation), unsigned, full CTest NOT re-run on the new presets (no C++ change). `docs/test-evidence/wave-b/`, ADR-008 |
| M8 HCS Vulkan kernels | **PARTIAL** (wave B / c1 adds 8 more kernels, see the section below; first kernel: correctness PASS, **not default-enabled**) | `src/kernels/{cpu,vulkan}` + `shaders/llm/gemv_q8_0.comp`: a Q8_0 GEMV, feature-flagged and OFF, with an independent CPU reference, SPIR-V compiled + `spirv-val`-validated + embedded at build time. 6/6 `kernel-cpu` + 4/4 `kernel-vulkan` on the real 680M; real-device CPU-vs-Vulkan match ~1e-5. Benchmark (isolated kernel): 2.1-2.5x faster than the single-threaded CPU reference at ~3.6 GiB/s on large shapes, slower on small ones - so it is **not** enabled. A v2 variant (shared-memory tiling of the activation vector) measured the same as v1 within noise: hypothesis **rejected**, v1 stays the default, v2 stays behind the `Variant` flag (`docs/research/vulkan-gemv-v2-shared-memory.md`). **Wave K (2026-11-20): upstream baseline measured and validation layers run.** GGML Vulkan Q8_0 `mul_mat_vec` 4096x14336 n=1 = 0.822 ms (70.7 GiB/s); the HCS kernel = 14.99 ms per call (3.88 GiB/s) = 18.2x slower (13.7x without the host weight copy), so it stays OFF. End to end, upstream Vulkan decode is 148.1 t/s (qwen3-0.6b Q4_K_M, CPU 75.1) and 39.7 t/s (LFM3-Omni-2B Q8_0, CPU 19.3), 5 reps median/p90. Validation layers found a real SPIR-V 1.5 on Vulkan 1.1 instance defect (fixed; now 0 messages). Device-lost + flag-off fallback tests added (12/12 kernel tests PASS). A tuned/resident-weight version, GPU timestamps and the autotuner cache are NOT RUN - `docs/test-evidence/vulkan-kernels/m8-baseline/`, `docs/research/vulkan-gemv-upstream-baseline.md`, `docs/vulkan-kernels.md` |
| M9 research features | NOT RUN | |
| M10 hardening: audit fixes (D1-D7) | PASS | `M10-fixes/` (red/green pairs): deep-JSON stack guard, connection budget/starvation, abandoned-request cancellation, runaway-job cap, reserved Windows device names, duplicate security headers, loopback literal parsing |
| M10 soak: 100-cycle load/unload, 30-min generation, long fuzz (ASan), OOM/device-lost/HDD-unplug injection, signing | PARTIAL: quick profile executed twice on the target machine, full profile (100 cycles, 30 min generation, long fuzz, signing) NOT RUN | `docs/test-evidence/soak/cpu-quick/` (first run: 12 gtests PASS, HttpStorm FAIL 2/18) and, after the wave-3 fixes, `cpu-quick-w3/` (12 gtests PASS, shutdown tests destroy the `Runtime` directly) + `cpu-quick-w3-httpstorm-rerun/` (HttpStorm 18/18 PASS) + `w3-ctrl-c/` (manual real CTRL_C_EVENT: exit rc=0 in 0.4 s while generating). Defects D-S1 (Runtime destructor crash), D-S2 (health starved by idle sockets), D-S3 (Ctrl+C: harness env problem + latent shutdown-order defect), D-S4 (slowloris deadline, new) are **fixed with red -> green tests**; D-S5 (`read_file_limited` torn read vs. concurrent `atomic_write_file`, **found by CI** on the W3 `FsUtil` test) is fixed with a single-handle read; D-S6 (every `--suite full` prefill point FAILed because a 1-token request reports 0 decode tokens) is fixed in the benchmark runner; details in `docs/test-evidence/soak/DEFECTS.md`. Wave G (2026-11-20) added simulated OOM, USB-unplug and shutdown tests (`wave-g/`; real RAM exhaustion and a physical cable pull NOT RUN). Not proven: full profile, Vulkan soak, a SIGINT path inside the automated harness (phase F uses CTRL_BREAK; SIGINT is a manual run) |
| Real Radeon 680M test | PASS for llama text (CPU **and** Vulkan), vision, diffusion, doctor; Vulkan text baseline B1 now recorded | `benchmark/`: Vulkan warm decode 126.1 t/s vs 66.1 t/s CPU, warm TTFT 134 ms vs 369 ms (same model/prompt/commit). Kernel tests: `kernel-cpu` + `kernel-vulkan` 12/12 PASS on the real 680M (wave R, `wave-r/ctest-kernels.txt`). The regression preset `windows-msvc-release` is CPU-only; the Vulkan preset has its own full run (wave A) |
| Text benchmark baselines | PASS (B0 CPU + B1 Vulkan, one controlled run each) | `benchmark/`: same model/prompt/parameters/commit, qwen3-0.6b Q4_K_M, cold+warm phases, full AGENTS.md 21.1 metadata. `--suite full` (prefill + decode sweeps) now has a real-model PASS run: `benchmark/full-suite-real.json` (LFM3-Omni-2B Q8_0, CPU; the earlier all-prefill-FAIL was the benchmark's 1-token handling bug, D-S6, fixed) |
| B0/B1 + M5 load-mode matrix (B4/B8 partial) | PASS (measured) | `M5-loadmodes/`: `auto`, `mmap`, `none`, `mlock`, `mmap+mlock`, `direct_io` on the real model from the USB drive, each with peak working set/commit; `--load-mode` is now exposed by `benchmark` and `vision` too (contract test) |
| B2/B5/B6 (best placement reference, LZ4/Zstd chunk loading) | NOT RUN | need the Model Vault in the load path (M5 Stage B) or a further implementation; B7 (diffusion compute vs parameter backend) is now measured in `B7-params/` |
| clang-format / clang-tidy | **PASS** (wave R: 4 wave-K kernel files were unformatted, fixed; format OK over 221 files, clang-tidy 0 diagnostics in 69 TUs - `docs/test-evidence/wave-r/`) | `scripts/format.ps1` green over 217 files with clang-format 19.1.5 (VS-bundled, pinned); `scripts/static-analysis.ps1 -Scope src -FailOnFindings` reports **0 diagnostics in 69 translation units** (Release). 2026-11-19: the hosted runner's VS 18 / MSVC 14.52 STL broke clang-tidy with `STL1000`; fixed by searching every VS install for the bundled LLVM and passing `-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` (`docs/static-analysis.md`). **V1.1 wave 1 (`6a390b8`) broke both gates on `main`** (CI run 44154743021): 4 files needed formatting and clang-tidy 20.1.8 found 6 `bugprone-implicit-widening-of-multiplication-result` sites in `include/hcs/{agent,api,train}` plus 1 `clang-analyzer` location inside `external/.../stb_image.h`. Fixed: widened the size constants first, and `static-analysis.ps1` now ignores diagnostics located under `external/` (the gate covers HCS-owned code). Evidence `docs/test-evidence/static-analysis/wave1-*`; triage in `docs/static-analysis.md` |
| Shader / SPIR-V validation | **PASS (one shader)** | `shaders/llm/gemv_q8_0.comp` is compiled by `glslc` and validated by `spirv-val --target-env vulkan1.1` (wave K: was 1.2, fixed after the validation layers flagged it) as part of the `windows-msvc-release-kernels` build (fails the build otherwise); the SPIR-V is embedded. No CI job runs it yet (the GPU/SDK job `hardware.yml` is NOT RUN) |
| CI workflows | **PASS: `pr-quick-gate` success on `0853143` and `release` success on `40bf581` (wave A a4, `gh run list`, `wave-a/a4/github-ci-state.txt`); `hardware.yml` never completed** | `.github/workflows/pr.yml` run 44167798984 (2026-11-17, commit `d8a5e37`): all six jobs green - `build + ctest` Release (18m38s) and Debug (9m59s), `clang-tidy` (5m53s), `clang-format` (17s), `SBOM` (17s), `web` (26s: 46 unit + static tests and the blocking headless boot check). Model-dependent tests skip on the hosted runner (NOT RUN), so CI proves configure/build/unit/contract/parser on a clean hosted machine, not Vulkan or real-model behaviour. History: run 44104062457 failed (Visual Studio not located, fixed in `fddc30d`); run 44154743021 (V1.1 wave 1) failed `clang-format` and `clang-tidy`, fixed in `06cfbe4` / `8b29907`. `hardware.yml` (self-hosted Radeon 680M runner): **NOT RUN, never executed** |
| Release workflow `release.yml` + `pr.yml` python/web-dist jobs (wave B) | **PASS (hosted)**: `release` run 44849519815 green on `40bf581` (draft pre-release with 8 assets, `wave-a/a4/github-ci-state.txt`); the earlier red run 44847528343 was fixed by `40bf581` | previously actionlint v1.7.7 only; Vulkan job is `continue-on-error` and its SDK install step is unproven on a hosted runner (`docs/test-evidence/wave-b/actionlint.txt`) |
| SBOM / checksums / package / wheels | PASS locally (wave B) / hosted-runner release NOT RUN | `scripts/release-local.ps1` produced `HCSInfer.exe`, `HCSInfer-vulkan.exe`, portable ZIP (both EXEs, 14 licence texts, manifest with dumpbin imports), `hcsinfer-1.0.0rc1-py3-none-win_amd64.whl` (48.99 MB; named `0.1.0` in the first wave-B run, before the version bump) + sdist (14.6 KB), web dist zip, CycloneDX SBOM (15 components), `SHA256SUMS.txt`; `twine check` PASSED both, fresh-venv install + real chat PASS; nothing uploaded. Signing NOT RUN, MSIX skipped. `docs/test-evidence/wave-b/`, `docs/packaging.md` |
| ADR-001..013 | all 13 written and accepted (001 scoped to SD1.5-class models, 004 with open validation items, 006 experimental, 008 a staged plan: the single-EXE claim is NOT made) | `docs/decisions/README.md` |

## Known limitations

- Engine model loads do **not** go through the Model Vault pipeline; the engines read the file themselves
  (mmap/engine loader). M5 Stage A (upstream load modes) is measured; Stage B needs a model that does not fit in RAM
  and a controlled cache state, neither of which is available on this machine.
- Image generation/editing: SD1.5 Q8_0 (256x256 smoke and 512x512 benchmark renders, CPU and Vulkan build) and FLUX.3 Klein
  512x512 (Vulkan text-to-image and image-to-image; CPU text-to-image only, n=1, no CPU image-to-image evidence), one image at
  a time; `url` responses, `mask`/inpainting and `multipart/form-data` uploads are rejected by design.
- Vision is limited to one image per single-turn request and two tested model families (SmolVLM2-500M, LFM3-Omni-2B).
- The GUI is proven over HTTP, a headless browser boot and (wave A, `docs/test-evidence/wave-a/gui-cpu`, `gui-vulkan`: 28 checks
  each) a browser interaction phase - load a model through the Models tab, streamed chat, Stop, no CSP violation - on the CPU
  **and** the Vulkan build. Not covered: image generation through the UI on the Vulkan build, screen-reader testing, a real
  clipboard click in a normal browser.
- One custom Vulkan kernel (Q8_0 GEMV, OFF by default, 18x slower than upstream GGML Vulkan), no scheduler beyond the single-worker JobQueue, no telemetry module.
- Cold SD1.5 first step took about 218 s when tensors streamed at 5-13 MB/s (`M2/image/README.md`); cold and warm
  numbers must not be compared.
- Vulkan diffusion output was once not byte-reproducible run to run (not investigated).
- Tests that need a real model skip (NOT RUN, never a fake pass) unless `HCS_TEST_GGUF` / `HCS_TEST_VLM` +
  `HCS_TEST_MMPROJ` / `HCS_TEST_DIFFUSION_MODEL` are set; Vulkan real tests need the Vulkan build.
- `hardware.yml` (self-hosted Radeon runner) has never completed a run (cancelled/pending only); real-GPU evidence comes from manual local runs only.
- `models load|unload` (a model lives in a running `serve`/MCP process, so the one-shot CLI cannot hold it) and `tune` are not CLI commands (exit code 10); load over HTTP or MCP.

## V1 stable release checklist (AGENTS.md section 22)

Derived only from evidence already in the repository; a gate with no evidence is NOT RUN. BLOCKED means a real
external dependency is missing. Wave references (W1..W6) are in `IMPLEMENTATION_PLAN.md`. Evidence paths are under
`docs/test-evidence/` unless written otherwise.

### Build and distribution

| Item | Status | Evidence / reason |
|---|---|---|
| Clean Windows x64 Release configure and build | PASS (hosted CI: `pr-quick-gate` success on `0853143`, `release` success on `40bf581`) | `wave-a/a4/github-ci-state.txt`; older run 44167798984 (Release leg) |
| Debug build and tests | PASS (hosted `pr-quick-gate`, Debug leg) | CI Debug leg; `M3-api-mcp/ctest-debug.txt` |
| Reproducible dependency pins and build command | PASS | `external/dependency-lock.json`, `docs/build.md`; caveat: Vulkan-Headers (FetchContent) is not in the lock file |
| No unexplained compiler/linker warnings in HCS code | PASS | `/W4 /WX` on HCS targets; clang-tidy gate reports 0 findings |
| Shader compilation and SPIR-V validation | PASS (one shader) | `glslc` + `spirv-val` run in the `windows-msvc-release-kernels` build; a failure fails the build (`docs/test-evidence/vulkan-kernels/`) |
| One-EXE requirement on a clean machine (or no single-file label) | **PARTIAL** (wave B: static Vulkan build + restricted-env runs of both EXEs PASS, see M7 row) (clean-machine NOT RUN; restricted-environment proxy PASS in wave G: the freshly built static EXE, sha256 `42067d40...`, ran alone with an empty PATH - help, doctor, a real CPU generation and `serve` health all PASS, and all 14 loaded modules are under `C:\Windows`; `wave-g/restricted-env-static-exe.json`. Windows Sandbox is not enabled and Hyper-V needs elevation, so no clean VM exists without changing system features) | A static-CRT `/MT` build now exists and its EXE depends on system DLLs only (`W4-static-exe/`), but the clean-machine test is NOT RUN and the release stays a portable folder not labelled single-file (ADR-008); W4 |
| No Vulkan SDK needed on the end-user machine | PASS for the kernels (SPIR-V embedded, `vulkan-1.dll` loaded dynamically); NOT RUN as a machine test | the kernel needs no SDK or import library at run time; no machine without the SDK was tested |
| External models and system AMD/Vulkan driver documented | PASS | `README.md`, `docs/hardware-support.md` |
| SHA-256 checksum and SBOM generated | **PASS (CI now produces + verifies them)** | the Release CI leg runs `scripts/package.ps1 -Zip`, verifies every `SHA256SUMS.txt` entry and starts the packaged `HCSInfer.exe`, and uploads the package + SBOM as artifacts (`package-*`); locally `docs/test-evidence/package/` (`SHA256SUMS`, CycloneDX SBOM) |
| Licence and third-party notices complete | **PARTIAL** | code dependencies verified against their licence files in `docs/licence-audit.md`; wave G added the prime-agent crate tree (345 crates + vendored `crossterm`: 0 licence mismatches against its own `deny.toml` allowlist). Still open: the literal `cargo deny` run, a Rust SBOM, and the per-model licence cards (weights never distributed) |

### Functional product

| Item | Status | Evidence / reason |
|---|---|---|
| Actual GGUF text generation | PASS | `M1/llama`, `M2/`, `benchmark/`, `lfm3/` |
| Actual supported VLM image analysis | PASS | `M2-vision/` (SmolVLM2-500M), `lfm3/` (LFM3-Omni-2B) |
| Actual diffusion image generation | PASS for SD1.5 Q8_0 and FLUX.3 Klein (substitute Qwen3-VL-4B encoder + FLUX.2 VAE; Vulkan t2i + i2i; CPU t2i only: 256x256 test plus one 512x512 benchmark sample; no CPU i2i evidence) | `M1/sd`, `M3-image/`; `wave-f/` |
| Model manager scan / inspect | PASS | `tests/contract/cli`, registry unit tests |
| Model load / unload | PASS over HTTP and MCP; FAIL as a CLI command (exit code 10, Planned) | `M3-api-mcp/`, `src/cli/cli.cpp` |
| Model switch and memory reclamation | PASS for switching and freeing the previous model; memory actually returned is NOT RUN | `M2/image/README.md`, `M3-image/` |
| GUI normal-user workflow | NOT RUN (automated); one manual run recorded | `M4-gui/README.md`, `studio/`; playwright phase not run |
| CLI workflows | PASS for the implemented commands; `tune`, `models load|unload` are not implemented; `cache` and `benchmark --suite full` are implemented (full: real-model run NOT RUN) | CLI row above (W1) |
| API real-inference tests | PASS (experimental) | `M3-api-mcp/`, `M3-image/`, `M2-vision/`, `lfm3/` |
| MCP stdio real-client tests | PASS with a scripted stdio client (`tests/e2e/api_mcp_real`); no third-party MCP client was run | `M3-api-mcp/`, `M3-image/` |
| CPU-only fallback | PASS | `M0/doctor-no-vulkan.json`, CPU runs of text, vision and diffusion |
| Clear unsupported model/backend errors | PASS | `Unsupported` errors (exit code 10 / HTTP 501), adapter tests |

### Storage and memory

| Item | Status | Evidence / reason |
|---|---|---|
| External USB HDD model root | PASS | `M0/readbench-usb-hdd.json`, `M5-loadmodes/` (model on the USB drive) |
| Bounded asynchronous loading/prefetch feeding the engines | BLOCKED | pipeline exists in `src/storage` but the engines do not load through it; no model larger than RAM and no admin rights to flush the file cache (M5 Stage B) |
| No HCS-owned decompressed SSD model cache by default | PASS (by construction) | no cache code exists (ADR-005); no test measures SSD writes |
| Windows page-file limitation documented | PASS | `docs/model-vault.md`, `README.md` |
| Corrupt/truncated model tests | PASS | `M6-storage/` (59 unit + 3 parser + 2 integration), `tests/unit/core/test_gguf_probe.cpp` |
| USB disconnect recovery | **PASS (simulated)** / physical unplug NOT RUN | wave G `tests/unit/storage/test_unplug.cpp` (writer source vanishing at every chunk boundary leaves no partial file and the name is reusable; invalid handle gives a structured `IoError`; a failed stream releases its handle) plus `test_source_faults.cpp`. A real cable pull during a load was not performed |
| Memory budget and OOM handling | **PASS (injected)** / real memory exhaustion NOT RUN | wave G `tests/contract/api/test_oom_injection.cpp` (8 tests: refused weights/KV reservation, engine-returned OOM, `std::bad_alloc` at load and at generate, refused model switch, 25 refusals in a row: ledger back to 0, state `Failed`/retryable, runtime still serves). `tests/unit/core/test_memory_budget.cpp` for the arithmetic. No test exhausts real RAM |
| Cold/warm load benchmarks | PASS for process-cold vs warm; page-cache-cold BLOCKED (needs administrator rights) | `benchmark/`, `M5-loadmodes/` |

### Compute correctness and performance

| Item | Status | Evidence / reason |
|---|---|---|
| Upstream CPU baseline | PASS | `benchmark/` (B0) |
| Upstream Vulkan baseline on the real Radeon 680M | PASS | `benchmark/` (B1), `B7-params/` |
| Real HCS kernel tests for each default-enabled kernel | n.a. (no kernel is default-enabled) | the one existing kernel (Q8_0 GEMV) is feature-flagged OFF; its `kernel-vulkan` tests are 4/4 PASS on the real 680M - `docs/test-evidence/vulkan-kernels/` |
| CPU/reference numeric correctness of HCS kernels | PASS (isolated kernel) | `hcs_kernels_cpu` reference + `KernelVulkanGemv.MatchesTheCpuReferenceAcrossShapes` (max abs err ~1e-5) |
| No default HCS kernel slower than its fallback | n.a. (nothing default-enabled) | the Q8_0 GEMV is slower than the CPU reference on small shapes and faster on large ones; it stays OFF |
| Scheduler decision and fallback observable | NOT RUN | no scheduler module; the kernel API returns `Unsupported` when off so a caller keeps the CPU path |
| Performance results reproducible and metadata-complete | PASS for B0/B1 (one machine); B2/B5/B6/B8 NOT RUN | `benchmark/` (AGENTS.md 21.1 JSON) |

### Security and reliability

| Item | Status | Evidence / reason |
|---|---|---|
| API localhost default and authentication | PASS | `tests/contract/api*`, `M10/security-audit.md`, `M10-fixes/` |
| MCP has no arbitrary shell/filesystem access | PASS | `tests/contract/mcp*`, `docs/mcp.md` |
| Origin validation for HTTP MCP | NOT RUN | Streamable HTTP is not implemented |
| Path traversal / symlink tests | PASS | `tests/unit/core/test_path_guard.cpp`, `M10-fixes/` |
| Parser fuzz smoke and scheduled fuzzing | smoke PASS (60 s, UBSan, no ASan); scheduled and long fuzz NOT RUN | `M6-storage/libfuzzer-run.txt` |
| Image size limits | PASS | `tests/contract/api_image`, `tests/contract/api_vision` |
| Cancellation and shutdown tests | **PASS** (fake engine for lifecycle logic + real model) | cancellation: adapter integration tests; shutdown while idle / loading / generating: `tests/contract/api_http/test_http_shutdown.cpp` (7) and, with the real LFM3-Omni-2B Q8_0 on the CPU engine behind the real HTTP server, `tests/integration/shutdown_real` (3 PASS, `wave-g/shutdown-real-model.txt`). A model load is not interruptible: shutdown waits for it (bounded by the load time) - asserted, not hidden. NOT RUN: shutdown on the Vulkan engine |
| Model switch / soak tests | PARTIAL (quick profile run once; full NOT RUN) | `scripts/soak.ps1`, `tests/soak/soak_core` (12 gtests; 11 need `HCS_SOAK_MODEL_ROOT`, skipped = NOT RUN without it) and `tests/soak/http_storm` are ported and build against `main`; they are not registered with CTest by default (`-DHCS_SOAK_REGISTER=ON`). The quick-profile run (model switch 3x, 8x load/unload, ...) is the M10 soak row above |
| No known critical vulnerability in bundled dependencies | **PARTIAL** | wave G (`wave-g/`): `npm audit` for `web/` and for the pinned runtime `@huggingface/transformers@4.3.2` = 0; `cargo audit` of prime-agent: 4 findings, all analysed in `docs/licence-audit.md` Findings 5 (3 not compiled in, 1 upstream-triaged); OSV query of the native pins: only **cpp-httplib** had advisories - fixed by the v0.44.0 bump (4 client/TLS ones remain, unreachable). NOT RUN: a CVE scanner over vendored stb/libwebp/oniguruma/utf8proc and over the engines' own code; OSV is not exhaustive |
| Privacy/logging policy documented | PASS | `README.md`, `SECURITY.md` |

### Documentation

| Item | Status | Evidence / reason |
|---|---|---|
| README feature matrix agrees with the tree | PASS by inspection (there is no automated check that README rows match the binary) | reconciled against the tree on 2026-11-18 and again in wave R (FLUX.3 and kernel rows corrected) by inspection (no clean-machine check) |
| Quick start tested from a clean machine | NOT RUN | |
| Supported models table | PASS | `docs/supported-models.md` |
| Troubleshooting | PASS | `README.md` |
| API and MCP docs | PASS | `docs/api.md`, `docs/mcp.md` |
| Model Vault docs | PASS | `docs/model-vault.md` |
| Hardware-support matrix | PASS | `docs/support-matrix.md` (backends, models, feature areas, one tested device) and `docs/hardware-support.md` |
| Benchmark methodology/results | PASS | `docs/benchmarks.md`, ADR-010 |
| Known limitations and non-goals | PASS | `README.md`, this file |
| Release notes | PASS (RC notes written; tag `v1.0.0-rc.1` is pushed, GitHub release is a **draft**) | `docs/release-notes-1.0.0-rc.1.md`, `CHANGELOG.md` 1.0.0-rc.1 entry; the tag/merge/push is the owner's step |

## Version decision (wave R, AGENTS.md section 22)

**Decision: `1.0.0-rc.1`, not `1.0.0`.** The rule was: 1.0.0 only if every core functional, security and packaging row of
the checklist above is PASS and the only open rows are owner-dependent. That is not the case, so the version stays a
release candidate. Classes of rows in the checklist tables above (a row with two statuses counts by its weaker half; no row count is claimed):

| Class | Notes |
|---|---|
| PASS (core and supporting) | build, text/vision/diffusion, API/MCP, CPU fallback, path/limit/cancel tests, docs rows, benchmarks, SBOM/checksums |
| PASS but simulated or partial coverage | USB disconnect (simulated), OOM (injected), shader validation (one shader), CI package |
| **Core rows that are PARTIAL / BLOCKED / NOT RUN and not owner-dependent** | see below |
| Owner-dependent or explicitly out of V1 scope | signing certificate, clean-machine VM, physical USB pull, hardware runner (`hardware.yml`), real WebGPU, Vercel deploy, hosted `release.yml` run, MCP Streamable HTTP + its Origin test (out of scope by design), literal `cargo deny` |

The core rows that keep this at rc: (1) **bounded asynchronous loading/prefetch feeding the engines**: BLOCKED
(M5 Stage B needs a model larger than RAM and an administrator page-cache flush); (2) **model switch / soak**: the full
profile (100 cycles, 30 minutes, long ASan fuzz) is NOT RUN, only the quick profile; (3) **GUI normal-user workflow**:
one manual run, the interactive automated phase is partial; (4) **no known critical vulnerability / licence notices**:
PARTIAL (one vendored-library scan and the literal `cargo deny` are NOT RUN); (5) **memory actually returned after a
model switch** and the scheduler row: NOT RUN (no scheduler module); (6) **parser fuzz**: 60 s smoke without ASan only.
The Radeon 680M gate, both static EXEs (not labelled single-file) (restricted-environment proxy), the wheel and the portable ZIP are PASS.
Version strings are consistent: CMake `HCS_VERSION_SUFFIX=rc.1` (`HCSInfer --version` = `HCSInfer 1.0.0-rc.1`, both
EXEs), wheel `1.0.0rc1`, `web/package.json` `1.0.0-rc.1`, README, CHANGELOG, release notes, SBOM.

## Release-readiness report (AGENTS.md section 23 format, wave R final)

```text
HCSInfer engineering report
- Current phase: wave R (release candidate) - final verification done
- Commit: binaries built from clean commit ae3bbbd (embedded ae3bbbdc4050, SBOM hcs:git_tree_dirty=false); later commits on this
  branch change only documentation, evidence and scripts/package.ps1. (Wave R verification was at e8bd522.) Branch claude/repo-analysis-v1-stable-d7c553 (later merged into main; tag v1.0.0-rc.1 pushed, draft release; see Wave A baseline).
- Build status: PASS - windows-msvc-release, -vulkan, -static (CPU) and -static-vulkan all build with /W4 /WX (static presets rebuilt
  from clean commit ae3bbbd in the audit-fix pass).
- Tests: PASS - audit pass on the static presets: CPU 812 passed, 4 skipped (816 registered), Vulkan 815 passed (815 registered), after
  serial re-runs of budget-refused first-run failures; wave R CPU build 811 passed, 0 failed, 4 skipped (815 registered; the 4 Vulkan-only tests are NOT RUN on a CPU build); Vulkan build 809/814 first run, the 5
  failures were out_of_memory refusals at ~1 GB free RAM and re-ran 6/6 PASS; web 46/46; wheel pytest 8/8;
  format OK (221 files); clang-tidy 0 diagnostics in 69 TUs; links 0 broken.
- Real Radeon 680M test: PASS (Vulkan CTest with the real models; benchmarks in docs/benchmarks.md).
- Real text model: PASS (LFM3-Omni-2B Q8_0, qwen3-0.6b Q4_K_M).
- Real VLM: PASS (SmolVLM2-500M, LFM3-Omni-2B; one image per request).
- Real diffusion model: PASS (SD1.5 Q8_0; FLUX.3 Klein UD-Q4_K_XL with substitute encoder/VAE, single samples).
- API/MCP: PASS (experimental; scripted stdio client, no third-party MCP client; Streamable HTTP not implemented).
- Storage/HDD: PARTIAL - raw reads and load modes measured, container tested; Stage B in the engine path BLOCKED,
  physical unplug NOT RUN.
- Security/licence: PARTIAL - audits and injected-fault tests PASS; signing, cargo deny, vendored-lib scans NOT RUN.
- Performance evidence: docs/benchmarks.md + docs/test-evidence/benchmarks/ (Vulkan about 2x decode, TTFT 2.8x (qwen3-0.6b) to 7.8x (LFM3) vs CPU;
  one machine, no thermal data).
- Known limitations: docs/release-notes-1.0.0-rc.1.md, README "Limitations".
- Remaining blockers: owner items (signing, clean-machine VM, hosted release.yml, hardware runner, WebGPU, Vercel,
  PyPI/GitHub release) and the six core rows above.
- Exact next action (superseded: merge and tag are done, the release is a draft; see PLAN_V1_STABLE_NEXT.md): owner publishes the draft and uploads the files in
  artifacts/release (hashes: docs/test-evidence/release-final/SHA256SUMS-final.txt); then run the soak full profile and the
  clean-machine test to move to 1.0.0.
```

Artefacts (EXEs built from the clean tree at `ae3bbbd`, the release set assembled by `scripts/release-local.ps1` from the final
branch head; final hashes in `docs/test-evidence/release-final/SHA256SUMS-final.txt`, LF line endings, every entry verified, portable ZIP
inner `SHA256SUMS.txt` verified): `HCSInfer.exe` (static CPU, 44,229,120 B, SHA-256 `d9b1a6bd...`), `HCSInfer-vulkan.exe` (static
CPU+Vulkan, 86,549,504 B, `a77cf42a...`), the portable ZIP (with README + docs essentials), `hcsinfer-1.0.0rc1-py3-none-win_amd64.whl` +
sdist, `hcs-studio-web-dist.zip`, `hcsinfer.cdx.json` (CycloneDX, 15 components). Nothing was uploaded at that time (the hosted release workflow later produced a draft release, see Wave A baseline). The earlier
`SHA256SUMS.txt` in that folder belongs to the superseded `e8bd522` build.

## Exact next action

**Wave R (1.0.0-rc.1) is done (2026-11-20)**, see the sections *Version decision* and *Release-readiness report* below, `docs/test-evidence/release-final/` and `docs/release-notes-1.0.0-rc.1.md`. (Superseded: the branch is merged into `main`, the tag `v1.0.0-rc.1` is pushed and the GitHub release is a draft; the owner still decides on publishing.) The remaining BLOCKED / NOT RUN items are listed in the release notes.

**Wave G is done (2026-11-20)**: simulated OOM / USB-unplug / shutdown gates PASS, supply-chain scan recorded (cpp-httplib bumped), restricted-environment static-EXE run PASS. Remaining owner-side or hardware-side items: a real clean Windows machine, a physical USB cable pull, real RAM exhaustion, the literal `cargo deny`, scans of the vendored C libraries, Debug/Vulkan presets against httplib v0.44.0, and the earlier-listed BLOCKED items below.

Execute the V1-stable wave plan in `IMPLEMENTATION_PLAN.md` in order. **W1** is done and committed (`902b41e`):
`POST /hcs/v1/benchmarks`, `cache stats|clear`, `benchmark --suite full`, and the MCP `hcs.runtime.benchmark` tool
(`af884c3`). **W2** (agent/train salvage) and **W3** (soak defects + test gaps) are done and committed (`5633450`,
`b02075f`). **W4** (static-CRT single-EXE attempt, ADR-008) is done as far as this machine allows: the
`windows-msvc-release-static` preset builds a system-DLL-only EXE that passes the full suite and runs real
llama/SD1.5 (`docs/test-evidence/W4-static-exe/`); the **clean-machine test and a `/MT` Vulkan build stay NOT RUN**.
**W5** (M8) has its first, feature-flagged kernel: a Q8_0 GEMV with CPU reference, SPIR-V validation and a real
device benchmark; it is **not default-enabled** (no measured benefit over the real baseline yet) and the upstream
Vulkan baseline was measured in **wave K** (the kernel is 18x slower than upstream, stays OFF; `docs/test-evidence/vulkan-kernels/m8-baseline/`). Next: **W6** (release candidate). These stay BLOCKED / NOT RUN until the owner acts: M5 Stage B (model larger than RAM, administrator cache flush), a real `prime-agent` session, real WebGPU
inference, the Vercel deploy and a clean Windows machine.

> Known provenance note: the already-built rc.1 portable ZIP embeds a manifest.json with the legacy flag `"single_exe": true`. This is superseded: `scripts/package.ps1` now emits `single_file_claim: false`. The release is NOT labelled single-file (clean-machine test NOT RUN). Regenerate the ZIP via `scripts/release-local.ps1` before publishing.


## Wave B / e1 (branch `wave-b/e1`): vault in load path, CLI load/unload/status, streaming, fast start
- `models load|unload|status` are HTTP clients of a running `serve` (PASS: 11 contract tests with real sockets + e2e with the real engine); `tune` was still exit 10 at that point (see wave B / c1).
- Vault stage in the load path: opt-in `serve --vault-prefetch`, default OFF. Real USB drive, N=3: cold load 56.2 s -> 33.6 s, warm 1.49 s -> 3.27 s (PASS as measured; see `docs/model-vault.md`). The engine still opens the path itself; streaming into engine buffers is NOT possible with the pinned loaders. Second drive E:, Vulkan, 50-client load test: NOT RUN.
- SSE: client disconnect mid-stream stops the real generation in 0.03 s (e2e PASS). Fast start: `models scan` 3.6 s -> 0.43 s on a 37k-file tree.
- Evidence: `docs/test-evidence/wave-b/e1/`.

## Wave B / c1 (branch `wave-b/c1`): HCSUDA port (caps, planner, oracle, 8 Vulkan kernels, `tune`)
- Build: `windows-msvc-release-kernels` preset, `/W4 /WX` clean, clang-format check OK (298 files). main's merge left a duplicate `bool lan` in `src/api/runtime.cpp` (build break); fixed here.
- Correctness PASS on the real Radeon 680M (driver 26.6.1, Vulkan 1.4.315): `hcs_test_vulkan_ops` 16/16 (kernel-vs-oracle on odd/tail/non-power-of-two shapes, 3 bit-identical runs, device-lost + timeout recovery, Khronos validation layer 0 messages); 10/10 `spirv-val`; planner/caps/oracle/profile unit tests PASS. Coopmat and int8-dot ran because the 680M reports `cooperative_matrix` and `integer_dot_product`.
- `HCSInfer tune --suite full` run for real; profile stored under `HCS_DATA_DIR`. Kernels are 1.3-26x a 16-thread CPU baseline (naive, not upstream). **PROMOTED** (correct + 3 identical runs + >=1.05x): all 8 kernels in at least one bucket; 5 buckets correct but excluded for timing noise. **DEFAULT-ENABLED: none** - the upstream GGML Vulkan op baseline is NOT RUN, so the planner keeps `reference` (CPU) for every op. RoPE trig error measured 4e-5 (512 rows) to 2e-4 (2048 rows) vs a 1e-3 tolerance; grows with position.
- Not in the engine path; no end-to-end speedup claim. Evidence: `docs/test-evidence/wave-b/c1/`, docs: `docs/vulkan-kernels.md`, `docs/hcsuda-port.md`, ADR-014.

## Wave C / k1 (branch `wave-c/k1`): in-process upstream GGML Vulkan baseline, kernel decision (M8, plan C3/C4)
- **Built:** `hcs_kernels_ggml_baseline` (one-node ggml graphs: `mul_mat` F32/F16/Q8_0/Q4_K, softmax, layernorm, rmsnorm, rope; spot-checked against a double reference), `hcs_ggml_vk_bench` tool, `tune` wiring (`upstream_ggml_vulkan` = measured / not_comparable / error / not_run; `--suite llm` with real LLM + SD shapes; `--no-upstream`), planner rule (upstream spread > 0.25 is not a result), `scripts/kernel-decision.ps1` (promotion across >= 3 runs), tests: `TuneCli.*` 4/4, `GgmlVkBaseline.*` 7/7 on the real 680M, planner/profile unit tests 34/34 (CPU). `/W4 /WX` clean, clang-format clean.
- **Measured (one complete `tune --suite llm` run, 680M, driver 26.6.1; baseline bench 15 samples):** HCS GEMM kernels are 0.20x-0.99x of upstream (2x-5x slower) on every real LLM/SD shape; softmax/RMSNorm/RoPE 0.7x-1.1x (parity); LayerNorm 0.89x-2.22x (4 of 5 buckets > 1.05x, fused vs upstream's 3-op chain) but 2 of those have an upstream spread > 0.25 and none was repeated. **Promoted: 0. Default-enabled: 0. Negative result recorded; every kernel stays OFF; no engine path, no GGML op override, no end-to-end claim** (nothing changed in text generation, so no token-identity run applies).
- **BLOCKED / NOT RUN:** the 3 whole-suite repeats with the final code (45-minute lock waits timed out twice: `D:\HCSScratch\realmodel.lock` was an orphaned 0-byte file from another agent's wrapper, not removed because it is not this task's lock); re-verification of the k>1024 oracle-data fix (the first run reported 5 GEMM buckets `FAIL` at k=4096 with unit-range data: fix = variance-preserving operand scale, tolerance untouched); the new LayerNorm model shapes (77x768, 729x1152, 4096x320) in the suite; a real OS TDR reset (only soft simulations); LoRA-merge GPU path (not implemented: output must stay bit-reproducible and the arithmetic is not the bottleneck - an estimate, not measured).
- `tune --reset` (drops one device's/kernel's saved evidence), TDR guard (1.5 s cold-run abort, ascending shapes, device-lost/timeout recovery tests) documented in `docs/vulkan-kernels.md`. Evidence: `docs/test-evidence/wave-c/k1/`.
