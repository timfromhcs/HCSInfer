# HCSInfer status

Last updated: 2026-11-17 (after the image-routes / embedded-UI / benchmark-command milestone and a full 400-test
Release run). Statuses: PASS / FAIL / BLOCKED / NOT RUN. Nothing below is claimed without an evidence file or an
inspected source file; items with no evidence are NOT RUN or BLOCKED.

## Repository state

- Git repo on branch `main`, pushed to the **public** remote `https://github.com/timfromhcs/HCSInfer` (created
  2026-11-17). Local and remote `main` are in sync; `main` is the only branch, so there is still no branch-to-branch
  diff. The history starts at `d5baf76` "Initial commit: HCSInfer core, engines, API, MCP, vault, vision, docs"
  (2026-11-17 13:57 +0100) and the work described below was committed on top of it in four further commits.
- Submodule pins: `external/llama.cpp` d3c3e242c16b8cbf51400fad410cd69b22d64f35, `external/stable-diffusion.cpp`
  532d2c15c430a785a712b2d94281f04e733008a4 (see `external/dependency-lock.json`).
- Toolchain on the target machine: MSVC 19.44.35228 (toolset 14.44.35207) x64, CMake 4.4.0, Ninja 1.13.2,
  Vulkan SDK 1.4.357.0 (build time only), Windows 11 build 10.0.26200.9974, Ryzen 7 7735HS, Radeon 680M
  (device id 0x1681), ~21.2 GB RAM, AMD driver 32.0.21043.12001.
- Modules present in `src/`: `core`, `hardware`, `storage`, `engines/llama`, `engines/diffusion`, `gui`, `api`,
  `mcp`, `cli`, `app`. **Still absent:** `memory`, `scheduler`, `telemetry`, `kernels`, `shaders`.
- The two smoke-test PNGs at the repository root (`cpu-256x256-2step.png`, `cpu-edit-256x256-2step.png`) are
  ignored by `.gitignore` (`/*.png`) and are not part of the product.

## Gate table

| Gate | Status | Evidence / reason |
|---|---|---|
| Full Release CTest suite (`build/img-api`, CPU-only Vulkan=OFF) | **PASS** | 400 tests: **396 passed, 0 failed, 4 skipped** (Vulkan real tests = NOT RUN on a CPU build), 223 s - `M10-fixes/ctest-full-400-green.txt` |
| Configure + build Debug / Release (MSVC x64, Ninja, `/W4 /WX` on HCS targets) | PASS | `M2-core/`, `M3-api-mcp/ctest-debug.txt`; the Vulkan preset configures and builds (`M2/image/README.md`) |
| M0 hardware doctor (CPU features, RAM, OS, Vulkan devices/heaps, GPU adapters, drive info, read bench) | PASS | `M0/` (`doctor.json`, `doctor-no-vulkan.json`, `readbench-usb-hdd.json`, `vulkaninfo-summary.txt`) |
| M1 llama.cpp independent baseline, CPU + Vulkan, real GGUF from USB drive | PASS | `M1/llama/README.md`; qwen3-0.6b Q4_K_M, sha256 `4ea79412...` |
| M1 stable-diffusion.cpp baseline, patched vs `SD_USE_UPSTREAM_GGML=ON`, CPU + Vulkan | PASS (SD1.5 Q8_0 only) | `M1/sd/README.md`; 512x512 output byte-identical between GGML modes once; model licence unverified |
| M1 multimodal (libmtmd) upstream proof | PARTIAL: NOT RUN as a standalone `llama-mtmd-cli` run; the pinned libmtmd is proven through the HCS adapter | `M2-vision/` (SmolVLM2-500M Q8_0 + mmproj, CPU and Vulkan) |
| M2 headless core (lifecycle, registry, JobQueue, MemoryBudget, PathGuard, settings) | PASS | unit tests; `M2-core/` |
| M2 control-plane facade: text or image models, switching, freeing the previous model | PASS (text and image, real) | `M2/llama-adapter-README.md`, `M2/image/README.md`, `M3-image/` |
| M2 llama adapter (text), CPU + Vulkan, cancellation, 10x load/unload | PASS | `M2/llama-engine-integration.txt` (16/16) |
| M2 llama adapter vision (`analyze_image`) | PASS (SmolVLM2-500M Q8_0 only; CPU and Vulkan) | `M2-vision/README.md`: 10/10 real-model engine tests on the Vulkan build including bad image / mismatched mmproj / cancel / reload |
| M2 diffusion adapter text-to-image + img2img, CPU + Vulkan | PASS | `M2/image/README.md`; integration tests `DiffusionBlocking.*` (6/6) now run with `HCS_TEST_DIFFUSION_MODEL` |
| CLI | PASS for `doctor`, `models scan/list/inspect/load/unload`, `chat`, `vision`, `image generate/edit`, `serve`, `mcp stdio`, `benchmark` | `tests/contract/cli`, `M2-vision/`, `benchmark/`; only `tune`, `cache` still return exit code 10 (planned) |
| `HCSInfer benchmark --suite quick` (real text benchmark, AGENTS.md 21.1 JSON) | PASS (one real CPU run) | `benchmark/`: B0-text, CPU, qwen3-0.6b Q4_K_M, cold+warm phases, full metadata, sha256 recorded. `--suite full` is NOT RUN (not implemented) |
| M3 HTTP API: `/v1/models`, `/v1/chat/completions` (+streaming), `/hcs/v1/{health,runtime,devices,memory,models,jobs}`, auth, loopback, limits | PASS (experimental) | `tests/contract/api*`, `M3-api-mcp/`, `M10-fixes/e2e-api-mcp-real-after-fix.json` |
| M3 HTTP image routes `/v1/images/generations`, `/v1/images/edits` | **PASS (experimental)** | `M3-image/`: 47/47 checks over a real socket with SD1.5 Q8_0 - real PNG of the requested size (not blank), edit keeps the input size, validation/limits, `multipart` -> 415, abandoned request cancels the render; CPU build only |
| HTTP vision input on `/v1/chat/completions` (`image_url` data URI, streaming, cancel) | PASS (experimental; SmolVLM2 only) | `M2-vision/` (`e2e_vision.py` 27/27), `tests/contract/api_vision`, `e2e_vision_real` |
| M3 MCP stdio (models, chat, runtime, jobs tools; path guard) | PASS (experimental) | `tests/contract/mcp`, `M10-fixes/e2e-api-mcp-real-after-fix.json` |
| MCP `hcs.vision.analyze` | PASS (experimental; real VLM over stdio) | `M2-vision/`, `tests/contract/api_vision` |
| MCP `hcs.image.generate`, `hcs.image.edit` | **PASS (experimental)** | `M3-image/`: real render, image returned as an MCP image content block, size kept, approved-root enforcement, cancellation |
| `POST /hcs/v1/benchmarks` | FAIL / not implemented | returns `501`; use the CLI `benchmark` command |
| MCP Streamable HTTP | NOT RUN (intentionally not implemented) | `docs/mcp.md` |
| M4 GUI (embedded local web UI, ADR-009 accepted) | **PASS (experimental)** for the HTTP/browser-boot phases | `M4-gui/`: 23 checks 0 failed over a real socket (CSP, traversal, Host/Origin, 405, token not embedded) **plus a real headless-Chrome boot** that connects with the fragment token and lists the models. Python-`playwright` interaction phase NOT RUN (playwright not installed); GUI not yet run on a Vulkan build |
| M4 normal-user workflow without a terminal | PARTIAL | manual run recorded in `M4-gui/README.md` (load, streamed chat, Stop/cancel, vision, Performance); not automated |
| M5 Model Vault v1 (bounded async read pipeline, HDD source, no SSD staging) | PARTIAL | `src/storage` (`file_source`, `stream`, `model_reader`) tested read-only on a real GGUF; **engines still do not load through the vault**; USB-unplug and cold e2e NOT RUN |
| M6 HCSMODEL container (header, chunk table, Raw/LZ4/Zstd, hashes, random access) | PASS (experimental) / PARTIAL for fuzz | 59 unit + 3 parser + 2 integration tests; libFuzzer smoke only 60 s with UBSan, no ASan (`M6-storage/`). Throughput benefit on a real HDD NOT proven |
| M7 shared GGML ABI (llama.cpp GGML + sd.cpp via `SD_USE_UPSTREAM_GGML=ON` in one binary) | PASS for build + SD1.5 + llama text in one binary | `HCSInfer.exe` links both; coverage for Flux/DiT/Qwen-Image, FP8/INT8 NOT RUN |
| M7 single `HCSInfer.exe`, static CRT, clean-machine test | NOT RUN | CRT is the default dynamic `/MD`; packaging is a portable folder only (ADR-008) |
| M8 HCS Vulkan kernels | NOT RUN | no `src/kernels`, no `shaders/` |
| M9 research features | NOT RUN | |
| M10 hardening: audit fixes (D1-D7) | PASS | `M10-fixes/` (red/green pairs): deep-JSON stack guard, connection budget/starvation, abandoned-request cancellation, runaway-job cap, reserved Windows device names, duplicate security headers, loopback literal parsing |
| M10 soak: 100-cycle load/unload, 30-min generation, long fuzz (ASan), OOM/device-lost/HDD-unplug injection, signing | NOT RUN | only 10x load/unload and the cancellation tests exist |
| Real Radeon 680M test | PASS for llama text (CPU **and** Vulkan), vision, diffusion, doctor; Vulkan text baseline B1 now recorded | `benchmark/`: Vulkan warm decode 126.1 t/s vs 66.1 t/s CPU, warm TTFT 134 ms vs 369 ms (same model/prompt/commit). Kernel tests NOT RUN; the 400-test suite is CPU-only because the regression preset is CPU-only |
| Text benchmark baselines | PASS (B0 CPU + B1 Vulkan, one controlled run each) | `benchmark/`: same model/prompt/parameters/commit, qwen3-0.6b Q4_K_M, cold+warm phases, full AGENTS.md 21.1 metadata. Other suites (B2-B8) NOT RUN; `--suite full` not implemented |
| B2-B8 (best-placement reference, vault/chunk storage, prefetch, sdcpp params) | NOT RUN | need the Model Vault in the load path (M5) or a further implementation |
| clang-format / clang-tidy | **PASS** | `scripts/format.ps1` green over 139 files with clang-format 19.1.5 (VS-bundled, pinned); `scripts/static-analysis.ps1` (clang-tidy 19.1.5) reports **0 diagnostics in 42 translation units** with the documented default check set, after the first full run found 81 and the real ones were fixed - triage in `docs/static-analysis.md` / `docs/test-evidence/static-analysis/` |
| Shader / SPIR-V validation | n.a. / NOT RUN | no shaders exist |
| CI workflows | **PASS (first green run)** | `.github/workflows/pr.yml` run 44105659773 (2026-11-17, commit `f9cd395`): all four jobs green - `clang-format check` (16 s), `SBOM generation + schema sanity` (11 s), `build + ctest (windows-msvc-release)` (16 m 45 s), `build + ctest (windows-msvc-debug)` (8 m 21 s). The two earlier runs failed: run 44104062457 in the build legs (Visual Studio not located on the runner, fixed in `fddc30d`) and run 44104849967 was cancelled by the next push. Model-dependent tests skip there (NOT RUN), so this proves configure/build/unit/contract/parser on a clean hosted machine, not Vulkan or real-model behaviour |
| SBOM / checksums / package | CREATED, NOT RUN in CI | `scripts/sbom.ps1`, `scripts/package.ps1` (portable folder, not single-EXE) |
| ADR-001..010 | ADR-005/006/007/009 accepted; 001-004, 008, 010 written/finalised | `docs/decisions/` |

## Known limitations

- Engine model loads do **not** go through the Model Vault pipeline; the engines read the file themselves
  (mmap/engine loader). M5 cold/warm vault benchmarks are therefore not meaningful yet.
- Image generation/editing is proven for SD1.5 Q8_0 at 256x256/2 steps on a CPU build only, one image at a time;
  `url` responses, `mask`/inpainting and `multipart/form-data` uploads are rejected by design.
- Vision is limited to one image per single-turn request and one tested model family (SmolVLM2).
- The GUI is proven over HTTP + a headless browser boot; the interactive `playwright` phase is NOT RUN on this
  machine (playwright not installed) and the UI has not been exercised on a Vulkan build.
- No custom kernels, no scheduler beyond the single-worker JobQueue, no telemetry module.
- Cold SD1.5 first step took about 218 s when tensors streamed at 5-13 MB/s (`M2/image/README.md`); cold and warm
  numbers must not be compared.
- Vulkan diffusion output was once not byte-reproducible run to run (not investigated).
- Tests that need a real model skip (NOT RUN, never a fake pass) unless `HCS_TEST_GGUF` / `HCS_TEST_VLM` +
  `HCS_TEST_MMPROJ` / `HCS_TEST_DIFFUSION_MODEL` are set; Vulkan real tests need the Vulkan build.
- There is no remote, so nothing is backed up and CI has never run.

## Exact next action

1. Watch the new `clang-tidy (static analysis)` CI job on the runner; flip it to blocking once it has been seen
   green there (the format gate is already blocking).
2. M1/M2 leftovers: standalone upstream `llama-mtmd-cli` baseline run; more VLM families (Qwen-VL M-RoPE, Gemma3);
   sdcpp `--params-backend disk` vs CPU comparison (B7).
3. M5: route engine model reads through the Model Vault, then cold/warm benchmarks and the USB-unplug test. This is
   the critical path to the remaining storage gates (B4/B5/B6/B8).
4. Run `static-analysis.ps1 -Scope all` over `tests/` and `tools/`, then the M10 soak tests (100 load/unload,
   30-minute generation, OOM/device-lost/HDD-unplug) and one profiled Vulkan kernel behind a flag (M8).
