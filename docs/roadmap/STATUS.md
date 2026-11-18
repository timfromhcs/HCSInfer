# HCSInfer status

Last updated: 2026-11-18 (docs reconciliation pass on `main` after V1.1 wave 2). Statuses: PASS / FAIL / BLOCKED /
NOT RUN. Nothing below is claimed without an evidence file or an inspected source file; items with no evidence are
NOT RUN or BLOCKED.

## Repository state

- Public remote `https://github.com/timfromhcs/HCSInfer`. Local `main` == `origin/main` == `35bd39d`
  ("feat(v1.1-agent): wave 2 - Windows process supervisor for the agent sidecar (ADR-011)"). The history starts at
  `d5baf76` "Initial commit" (2026-11-17) and has 28 commits, all dated 2026-11-17.
- CI: `.github/workflows/pr.yml` run **44167798984** (commit `d8a5e37`) is green on all six jobs (see the `CI workflows`
  row). `hardware.yml` (self-hosted Radeon runner) has never been executed.
- Branches: `main` and the docs worktree branch `claude/repo-analysis-v1-stable-80f8d6` (== `main`). The local branch
  `claude/flux3-agent-engine-v1-665ab0` is **8 commits behind `main`** and its worktree holds **uncommitted work**
  that is being salvaged selectively (it is not merged and nothing from it is claimed here).
- Submodule pins: `external/llama.cpp` d3c3e242c16b8cbf51400fad410cd69b22d64f35, `external/stable-diffusion.cpp`
  532d2c15c430a785a712b2d94281f04e733008a4, `external/prime-agent` 8980e66f2f07762d62a6c1d73b347a4cac5275cc
  (see `external/dependency-lock.json`).
- Toolchain on the target machine: MSVC 19.44.35228 (toolset 14.44.35207) x64, CMake 4.4.0, Ninja 1.13.2,
  Vulkan SDK 1.4.357.0 (build time only), Windows 11 build 10.0.26200.9974, Ryzen 7 7735HS, Radeon 680M
  (device id 0x1681), ~21.2 GB RAM, AMD driver 32.0.21043.12001.
- Modules present in `src/`: `core`, `hardware`, `storage`, `agent`, `train`, `process`, `engines/llama`,
  `engines/diffusion`, `gui`, `api`, `mcp`, `cli`, `app`. **Still absent:** `memory`, `scheduler`, `telemetry`,
  `kernels`, `shaders`. A separate browser deliverable `web/` (HCS Studio Web, ADR-013) exists outside `src/` and
  outside the CMake graph.
- The two smoke-test PNGs at the repository root (`cpu-256x256-2step.png`, `cpu-edit-256x256-2step.png`) are
  ignored by `.gitignore` (`/*.png`) and are not part of the product.

## Gate table

| Gate | Status | Evidence / reason |
|---|---|---|
| Full Release CTest suite (CPU-only, Vulkan=OFF) | **PASS** | **550 registered, 0 failed at the last run** (W1, CPU build, model-dependent tests skip = NOT RUN; earlier: 518 at wave 2, `docs/test-evidence/agent/README.md`). The last archived raw log is the earlier 400-test run: 396 passed, 0 failed, 4 skipped (Vulkan real tests = NOT RUN on a CPU build), 223 s - `M10-fixes/ctest-full-400-green.txt`; the 518-test raw log is not archived in `docs/test-evidence/`. Model-dependent tests skip (NOT RUN) unless `HCS_TEST_*` is set |
| Configure + build Debug / Release (MSVC x64, Ninja, `/W4 /WX` on HCS targets) | PASS | `M2-core/`, `M3-api-mcp/ctest-debug.txt`; the Vulkan preset configures and builds (`M2/image/README.md`) |
| M0 hardware doctor (CPU features, RAM, OS, Vulkan devices/heaps, GPU adapters, drive info, read bench) | PASS | `M0/` (`doctor.json`, `doctor-no-vulkan.json`, `readbench-usb-hdd.json`, `vulkaninfo-summary.txt`) |
| M1 llama.cpp independent baseline, CPU + Vulkan, real GGUF from USB drive | PASS | `M1/llama/README.md`; qwen3-0.6b Q4_K_M, sha256 `4ea79412...` |
| M1 stable-diffusion.cpp baseline, patched vs `SD_USE_UPSTREAM_GGML=ON`, CPU + Vulkan | PASS (SD1.5 Q8_0 only) | `M1/sd/README.md`; 512x512 output byte-identical between GGML modes once; model licence unverified |
| B7 compute backend vs parameter backend (sdcpp `--backend` / `--params-backend`) | PASS (measured, one sample each) | `B7-params/`: vulkan0+cpu-params 5.22 s / 1773 MB peak WS, vulkan0+disk-params 3.29 s / 625 MB, cpu+cpu 9.68 s; outputs differ between placements (equivalence NOT verified); the model is on internal NVMe, so a USB-HDD disk-params case is NOT RUN |
| M1 multimodal (libmtmd) upstream proof | PARTIAL: NOT RUN as a standalone `llama-mtmd-cli` run; the pinned libmtmd is proven through the HCS adapter | `M2-vision/` (SmolVLM2-500M Q8_0 + mmproj, CPU and Vulkan) |
| M2 headless core (lifecycle, registry, JobQueue, MemoryBudget, PathGuard, settings) | PASS | unit tests; `M2-core/` |
| M2 control-plane facade: text or image models, switching, freeing the previous model | PASS (text and image, real) | `M2/llama-adapter-README.md`, `M2/image/README.md`, `M3-image/` |
| M2 llama adapter (text), CPU + Vulkan, cancellation, 10x load/unload | PASS | `M2/llama-engine-integration.txt` (16/16) |
| M2 llama adapter vision (`analyze_image`) | PASS (SmolVLM2-500M Q8_0 only; CPU and Vulkan) | `M2-vision/README.md`: 10/10 real-model engine tests on the Vulkan build including bad image / mismatched mmproj / cancel / reload |
| M2 diffusion adapter text-to-image + img2img, CPU + Vulkan | PASS | `M2/image/README.md`; integration tests `DiffusionBlocking.*` (6/6) now run with `HCS_TEST_DIFFUSION_MODEL` |
| CLI | PASS for `doctor`, `models scan/list/inspect`, `chat`, `vision`, `image generate/edit`, `serve`, `mcp stdio`, `benchmark --suite quick|full`, `cache stats|clear`, `train validate/gate/adapter/pipeline` | `tests/contract/cli`, `M2-vision/`, `benchmark/`, `docs/test-evidence/train/cli-demo.txt`. **`models load`, `models unload` and `tune` return exit code 10 (Planned)** (`src/cli/cli.cpp`); model load/unload works over HTTP and MCP |
| `HCSInfer benchmark --suite quick` (real text benchmark, AGENTS.md 21.1 JSON) | PASS (one real CPU run) | `benchmark/`: B0-text, CPU, qwen3-0.6b Q4_K_M, cold+warm phases, full metadata, sha256 recorded. `--suite full` is implemented and contract-tested but no real-model run is recorded (NOT RUN) |
| M3 HTTP API: `/v1/models`, `/v1/chat/completions` (+streaming, OpenAI `tools` / `tool_calls`, `tool_choice` auto or none), `/hcs/v1/{health,runtime,devices,memory,models,jobs}`, auth, loopback, limits | PASS (experimental) | `tests/contract/api*`, `M3-api-mcp/`, `M10-fixes/e2e-api-mcp-real-after-fix.json`; tool calling: `lfm3/` (19 unit + 12 contract tests, real model in `e2e-lfm3-*.json`; `tool_choice` required / named function and legacy `functions` answer 501) |
| M3 HTTP image routes `/v1/images/generations`, `/v1/images/edits` | **PASS (experimental)** | `M3-image/`: 47/47 checks over a real socket with SD1.5 Q8_0 on the **CPU build and the Vulkan build** (`vulkan-e2e-image.json`, plus the four `vulkan-*.png` renders) - real PNG of the requested size (not blank), edit keeps the input size, validation/limits, `multipart` -> 415, abandoned request cancels the render |
| HTTP vision input on `/v1/chat/completions` (`image_url` data URI, streaming, cancel) | PASS (experimental; SmolVLM2 only) | `M2-vision/` (`e2e_vision.py` 27/27), `tests/contract/api_vision`, `e2e_vision_real` |
| M3 MCP stdio (models, chat, runtime, jobs tools; path guard) | PASS (experimental) | `tests/contract/mcp`, `M10-fixes/e2e-api-mcp-real-after-fix.json` |
| MCP `hcs.vision.analyze` | PASS (experimental; real VLM over stdio) | `M2-vision/`, `tests/contract/api_vision` |
| MCP `hcs.image.generate`, `hcs.image.edit` | **PASS (experimental)** | `M3-image/`: real render, image returned as an MCP image content block, size kept, approved-root enforcement, cancellation |
| `POST /hcs/v1/benchmarks` | PASS (contract tests, no model) | `202` + job, result via `GET /hcs/v1/jobs/{id}`, cancel, `404`/`400`/`501`, body <= 4 KiB (`tests/contract/api/test_http_benchmarks.cpp`); real-model run NOT RUN |
| MCP Streamable HTTP | NOT RUN (intentionally not implemented) | `docs/mcp.md` |
| M4 GUI (embedded local web UI, ADR-009 accepted) | **PASS (experimental)** for the HTTP/browser-boot phases, on both builds | `M4-gui/`: 23 checks 0 failed over a real socket (CSP, traversal, Host/Origin, 405, token not embedded) **plus a real headless-Chrome boot** that connects with the fragment token and lists the models; the same script against the **Vulkan build** is `vulkan-e2e-gui.json` (22 checks, 0 failed - one check needs a registered model). Python-`playwright` interaction phase NOT RUN (playwright not installed) |
| M4 normal-user workflow without a terminal | PARTIAL | manual run recorded in `M4-gui/README.md` (load, streamed chat, Stop/cancel, vision, Performance); not automated |
| V1.1 wave 1: LFM3-Omni tool calls + agentic eval, FLUX.3 Klein plumbing + presets, HCS Studio UI, agent/train foundations | PASS (experimental) | `docs/test-evidence/lfm3`, `flux3`, `agent`, `studio`; the FLUX.3 render stays BLOCKED (gated encoder/VAE). Commit `6a390b8` (V1.1 wave 1) |
| V1.1 wave 2: HCS Agent foundations (policy, loopback provider document, launch plan, Windows process supervisor, ADR-011) | PASS (logic + supervision) / real agent run NOT RUN | `hcs_agent` gained `agent_config` (loopback-only provider document + a launch plan scoped to an approved root, `--offline`) and a new `hcs_process` module (spawn without a shell, bounded merged-output capture, timeout, whole-tree kill via a kill-on-close Job Object). 8 `ChildProcess.*` tests run against a real helper executable, including a grandchild proven dead after the job is closed. Full suite **518/518**; clang-tidy clean over 57 TUs. **NOT RUN**: a real `prime-agent` session and the venv bootstrap. `docs/agent.md`, `docs/test-evidence/agent/` |
| V1.1 wave 2: HCS Train pipeline (evaluation-report ingestion, eval gate, adapter book, `HCSInfer train` CLI, ADR-012) | PASS (logic + CLI) / real training NOT RUN | `hcs_train` gained `eval_report`, `adapter_book`, `pipeline` and the `train` CLI. 25 unit + 5 CLI contract tests; full Release suite **505/505, 0 failed**; clang-tidy clean over 55 TUs; clang-format clean over 164 files. Promotion is refused unless the gate kept the adapter (exit 11); reports and manifests are parsed fail-closed and bounded; ids cannot escape the book. No trainer run and no real adapter exist. `docs/train.md`, `docs/test-evidence/train/` (`cli-demo.txt`) |
| V1.1 wave 2: HCS Studio Web (`web/`, browser WebGPU/WASM, ADR-013) | PASS for the shell + logic / real WebGPU inference NOT RUN | `web/`: 46 unit + static security tests (`npm test`, 46/46) and `tests/e2e/boot.mjs` on a real static server + real headless Chrome (14/14, `docs/test-evidence/web/web-boot.{json,txt}` + `web-chat.png`); the headless browser has no WebGPU adapter so it fell back to the WASM path. CI run 44167798984 is green on all six jobs; the headless step found that a browser can expose `navigator.gpu` while `requestAdapter()` never settles, so the probe now times out (4 s) and falls back instead of hanging. Both web steps are blocking gates. Image Studio/Train/Agent are "Local only"; Vercel deploy NOT RUN (needs owner credentials) |
| M5 Model Vault v1 | PARTIAL | Stage A (upstream load modes) **measured**: all six llama.cpp modes work; `direct_io` costs ~14x load time on the USB HDD because it bypasses the file cache, mmap-backed modes commit ~250 MB less than read-into-RAM, no throughput difference while the model fits in RAM - `M5-loadmodes/`. Stage B (bounded async pipeline feeding the engine) **BLOCKED**: no model larger than the 21 GB RAM is available locally and the OS file cache cannot be flushed without administrator rights, so a controlled storage-bound comparison cannot be produced on this machine; `src/storage` reads are tested read-only on a real GGUF, engines do not load through the vault. USB-unplug test NOT RUN |
| M6 HCSMODEL container (header, chunk table, Raw/LZ4/Zstd, hashes, random access) | PASS (experimental) / PARTIAL for fuzz | 59 unit + 3 parser + 2 integration tests; libFuzzer smoke only 60 s with UBSan, no ASan (`M6-storage/`). Throughput benefit on a real HDD NOT proven |
| M7 shared GGML ABI (llama.cpp GGML + sd.cpp via `SD_USE_UPSTREAM_GGML=ON` in one binary) | PASS for build + SD1.5 + llama text in one binary | `HCSInfer.exe` links both; coverage for Flux/DiT/Qwen-Image, FP8/INT8 NOT RUN |
| M7 single `HCSInfer.exe`, static CRT, clean-machine test | NOT RUN | CRT is the default dynamic `/MD`; packaging is a portable folder only (ADR-008) |
| M8 HCS Vulkan kernels | NOT RUN | no `src/kernels`, no `shaders/` |
| M9 research features | NOT RUN | |
| M10 hardening: audit fixes (D1-D7) | PASS | `M10-fixes/` (red/green pairs): deep-JSON stack guard, connection budget/starvation, abandoned-request cancellation, runaway-job cap, reserved Windows device names, duplicate security headers, loopback literal parsing |
| M10 soak: 100-cycle load/unload, 30-min generation, long fuzz (ASan), OOM/device-lost/HDD-unplug injection, signing | NOT RUN | only 10x load/unload and the cancellation tests exist |
| Real Radeon 680M test | PASS for llama text (CPU **and** Vulkan), vision, diffusion, doctor; Vulkan text baseline B1 now recorded | `benchmark/`: Vulkan warm decode 126.1 t/s vs 66.1 t/s CPU, warm TTFT 134 ms vs 369 ms (same model/prompt/commit). Kernel tests NOT RUN; the 400-test suite is CPU-only because the regression preset is CPU-only |
| Text benchmark baselines | PASS (B0 CPU + B1 Vulkan, one controlled run each) | `benchmark/`: same model/prompt/parameters/commit, qwen3-0.6b Q4_K_M, cold+warm phases, full AGENTS.md 21.1 metadata. Prefill/decode sweeps (`--suite full`) implemented, NOT RUN on a real model |
| B0/B1 + M5 load-mode matrix (B4/B8 partial) | PASS (measured) | `M5-loadmodes/`: `auto`, `mmap`, `none`, `mlock`, `mmap+mlock`, `direct_io` on the real model from the USB drive, each with peak working set/commit; `--load-mode` is now exposed by `benchmark` and `vision` too (contract test) |
| B2/B5/B6 (best placement reference, LZ4/Zstd chunk loading) | NOT RUN | need the Model Vault in the load path (M5 Stage B) or a further implementation; B7 (diffusion compute vs parameter backend) is now measured in `B7-params/` |
| clang-format / clang-tidy | **PASS** (after fixing a wave-1 regression) | `scripts/format.ps1` green over 152 files with clang-format 19.1.5 (VS-bundled, pinned); `scripts/static-analysis.ps1 -Scope src -FailOnFindings` reports **0 diagnostics in 51 translation units**. **V1.1 wave 1 (`6a390b8`) broke both gates on `main`** (CI run 44154743021): 4 files needed formatting and clang-tidy 20.1.8 found 6 `bugprone-implicit-widening-of-multiplication-result` sites in `include/hcs/{agent,api,train}` plus 1 `clang-analyzer` location inside `external/.../stb_image.h`. Fixed: widened the size constants first, and `static-analysis.ps1` now ignores diagnostics located under `external/` (the gate covers HCS-owned code). Evidence `docs/test-evidence/static-analysis/wave1-*`; triage in `docs/static-analysis.md` |
| Shader / SPIR-V validation | n.a. / NOT RUN | no shaders exist |
| CI workflows | **PASS (green on `main`)** | `.github/workflows/pr.yml` run 44167798984 (2026-11-17, commit `d8a5e37`): all six jobs green - `build + ctest` Release (18m38s) and Debug (9m59s), `clang-tidy` (5m53s), `clang-format` (17s), `SBOM` (17s), `web` (26s: 46 unit + static tests and the blocking headless boot check). Model-dependent tests skip on the hosted runner (NOT RUN), so CI proves configure/build/unit/contract/parser on a clean hosted machine, not Vulkan or real-model behaviour. History: run 44104062457 failed (Visual Studio not located, fixed in `fddc30d`); run 44154743021 (V1.1 wave 1) failed `clang-format` and `clang-tidy`, fixed in `06cfbe4` / `8b29907`. `hardware.yml` (self-hosted Radeon 680M runner): **NOT RUN, never executed** |
| SBOM / checksums / package | PASS (portable folder + ZIP) / single-EXE NOT RUN | `scripts/package.ps1 -Preset img-api -Zip` produced `HCSInfer-0.1.0-img-api-portable` (21 files, 12 licence texts, manifest, CycloneDX SBOM with 13 components, SHA256SUMS over 20 files) and a 17.2 MiB ZIP with SHA-256 `527df693...`; the packaged `HCSInfer.exe` runs standalone (`doctor --json`, exit 0) - `M10-fixes/../package/`. Still open: clean-machine test without the MSVC redistributable, signing, single-EXE |
| ADR-001..013 | all 13 written and accepted (001 scoped to SD1.5-class models, 004 with open validation items, 006 experimental, 008 a staged plan: the single-EXE claim is NOT made) | `docs/decisions/README.md` |

## Known limitations

- Engine model loads do **not** go through the Model Vault pipeline; the engines read the file themselves
  (mmap/engine loader). M5 Stage A (upstream load modes) is measured; Stage B needs a model that does not fit in RAM
  and a controlled cache state, neither of which is available on this machine.
- Image generation/editing is proven for SD1.5 Q8_0 at 256x256/2 steps on the CPU **and** Vulkan build, one image at
  a time; `url` responses, `mask`/inpainting and `multipart/form-data` uploads are rejected by design.
- Vision is limited to one image per single-turn request and one tested model family (SmolVLM2).
- The GUI is proven over HTTP + a headless browser boot; the interactive `playwright` phase is NOT RUN on this
  machine (playwright not installed) and the UI has not been exercised on a Vulkan build.
- No custom kernels, no scheduler beyond the single-worker JobQueue, no telemetry module.
- Cold SD1.5 first step took about 218 s when tensors streamed at 5-13 MB/s (`M2/image/README.md`); cold and warm
  numbers must not be compared.
- Vulkan diffusion output was once not byte-reproducible run to run (not investigated).
- Tests that need a real model skip (NOT RUN, never a fake pass) unless `HCS_TEST_GGUF` / `HCS_TEST_VLM` +
  `HCS_TEST_MMPROJ` / `HCS_TEST_DIFFUSION_MODEL` are set; Vulkan real tests need the Vulkan build.
- `hardware.yml` (self-hosted Radeon runner) has never run; real-GPU evidence comes from manual local runs only.
- `models load|unload` and `tune` CLI commands are not implemented (exit code 10).

## V1 stable release checklist (AGENTS.md section 22)

Derived only from evidence already in the repository; a gate with no evidence is NOT RUN. BLOCKED means a real
external dependency is missing. Wave references (W1..W6) are in `IMPLEMENTATION_PLAN.md`. Evidence paths are under
`docs/test-evidence/` unless written otherwise.

### Build and distribution

| Item | Status | Evidence / reason |
|---|---|---|
| Clean Windows x64 Release configure and build | PASS | CI run 44167798984 (hosted clean runner, Release leg) |
| Debug build and tests | PASS | CI Debug leg; `M3-api-mcp/ctest-debug.txt` |
| Reproducible dependency pins and build command | PASS | `external/dependency-lock.json`, `docs/build.md`; caveat: Vulkan-Headers (FetchContent) is not in the lock file |
| No unexplained compiler/linker warnings in HCS code | PASS | `/W4 /WX` on HCS targets; clang-tidy gate reports 0 findings |
| Shader compilation and SPIR-V validation | NOT RUN | no shaders exist |
| One-EXE requirement on a clean machine (or no single-file label) | NOT RUN | dynamic CRT; only a portable folder exists and it is not labelled single-file (ADR-008); W4 |
| No Vulkan SDK needed on the end-user machine | NOT RUN | Vulkan loads from the system driver by design, but no machine without the SDK was tested |
| External models and system AMD/Vulkan driver documented | PASS | `README.md`, `docs/hardware-support.md` |
| SHA-256 checksum and SBOM generated | PASS | `package/` (SHA256SUMS, CycloneDX SBOM); not produced by a release CI job yet (W6) |
| Licence and third-party notices complete | NOT RUN | `THIRD_PARTY_NOTICES.md` is an inventory; the formal licence review and the prime-agent crate audit are open |

### Functional product

| Item | Status | Evidence / reason |
|---|---|---|
| Actual GGUF text generation | PASS | `M1/llama`, `M2/`, `benchmark/`, `lfm3/` |
| Actual supported VLM image analysis | PASS | `M2-vision/` (SmolVLM2-500M), `lfm3/` (LFM3-Omni-2B) |
| Actual diffusion image generation | PASS for SD1.5 Q8_0; FLUX.3 Klein render BLOCKED | `M1/sd`, `M3-image/`; `flux3/` (gated encoder/VAE not obtainable) |
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
| USB disconnect recovery | NOT RUN | W3 |
| Memory budget and OOM handling | NOT RUN for OOM injection (budget arithmetic is unit-tested) | `tests/unit/core/test_memory_budget.cpp`; W3 |
| Cold/warm load benchmarks | PASS for process-cold vs warm; page-cache-cold BLOCKED (needs administrator rights) | `benchmark/`, `M5-loadmodes/` |

### Compute correctness and performance

| Item | Status | Evidence / reason |
|---|---|---|
| Upstream CPU baseline | PASS | `benchmark/` (B0) |
| Upstream Vulkan baseline on the real Radeon 680M | PASS | `benchmark/` (B1), `B7-params/` |
| Real HCS kernel tests for each default-enabled kernel | NOT RUN | no HCS kernel exists (W5) |
| CPU/reference numeric correctness of HCS kernels | NOT RUN | no kernel exists |
| No default HCS kernel slower than its fallback | NOT RUN | no kernel exists |
| Scheduler decision and fallback observable | NOT RUN | no scheduler module |
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
| Cancellation and shutdown tests | PASS for cancellation; shutdown while idle / loading / generating NOT RUN | adapter integration tests |
| Model switch / soak tests | NOT RUN | only 10x load/unload exists (W3) |
| No known critical vulnerability in bundled dependencies | NOT RUN | no vulnerability scan was run |
| Privacy/logging policy documented | PASS | `README.md`, `SECURITY.md` |

### Documentation

| Item | Status | Evidence / reason |
|---|---|---|
| README feature matrix matches the binary | PASS | reconciled against the tree on 2026-11-18 by inspection (no clean-machine check) |
| Quick start tested from a clean machine | NOT RUN | |
| Supported models table | PASS | `docs/supported-models.md` |
| Troubleshooting | PASS | `README.md` |
| API and MCP docs | PASS | `docs/api.md`, `docs/mcp.md` |
| Model Vault docs | PASS | `docs/model-vault.md` |
| Hardware-support matrix | PASS | `docs/hardware-support.md` (one tested device) |
| Benchmark methodology/results | PASS | `docs/benchmarks.md`, ADR-010 |
| Known limitations and non-goals | PASS | `README.md`, this file |
| Release notes | NOT RUN | `CHANGELOG.md` exists; release notes and the support matrix are W6 |

## Exact next action

Execute the V1-stable wave plan in `IMPLEMENTATION_PLAN.md` in order: **W1** (done in the working tree, uncommitted: `POST /hcs/v1/benchmarks`, `cache stats|clear`, `benchmark --suite full`; `tune` stays Planned), **W2** (salvage the
unique agent/train work from the `flux3-agent-engine` worktree, with tests), **W3** (lifecycle hardening, soak, fault
injection), **W4** (single-EXE static-CRT attempt, ADR-008), **W5** (M8 profiling, at most one feature-flagged
kernel), **W6** (release candidate). These stay BLOCKED / NOT RUN until the owner acts: the FLUX.3 render (gated
encoder/VAE access), M5 Stage B (model larger than RAM, administrator cache flush), a real `prime-agent` session,
real WebGPU inference and the Vercel deploy.
