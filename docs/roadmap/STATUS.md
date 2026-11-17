# HCSInfer status

Last updated: 2026-11-17 (after the image-routes / embedded-UI / benchmark-command milestone and a full 400-test
Release run). Statuses: PASS / FAIL / BLOCKED / NOT RUN. Nothing below is claimed without an evidence file or an
inspected source file; items with no evidence are NOT RUN or BLOCKED.

## Repository state

- Git repo on branch `main` with exactly one commit, `d5baf76` "Initial commit: HCSInfer core, engines, API, MCP,
  vault, vision, docs" (2026-11-17 13:57 +0100). The work described below was in the working tree on top of it and is
  committed with this status update.
- **No remote is configured and no GitHub repository exists for HCSInfer** (`git remote -v` is empty; the only
  `github.com` repos on the `timfromhcs` account are unrelated projects). There is therefore **no local-vs-GitHub
  diff** and no other branch to compare: `main` is the only branch (local and remote). CI workflows exist but have
  never executed; they need a pushed repository.
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
| Real Radeon 680M test | PASS for llama text, vision, diffusion, doctor (earlier evidence) | kernel tests NOT RUN; the final 400-test run above is CPU-only |
| B0 CPU text baseline | PASS (one controlled run) | `benchmark/` (`benchmark_id B0-text`) |
| B1 Vulkan text baseline, B2-B8 | NOT RUN | no Vulkan run in the final evidence set |
| clang-format / clang-tidy | NOT RUN | `scripts/format.ps1` exists, no formatting pass has been applied; no compile database run recorded |
| Shader / SPIR-V validation | n.a. / NOT RUN | no shaders exist |
| CI workflows | CREATED, NEVER RUN | `.github/workflows/pr.yml`, `hardware.yml`; **no remote**, so they cannot run yet |
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

1. Create a GitHub repository for this project and push `main` (needs the user's decision: public or private), so the
   two workflows run for the first time and the work is backed up.
2. M1/M2 leftovers: standalone upstream `llama-mtmd-cli` baseline run; more VLM families (Qwen-VL M-RoPE, Gemma3);
   sdcpp `--params-backend disk` vs CPU comparison (B7).
3. M5: route engine model reads through the Model Vault, then cold/warm benchmarks and the USB-unplug test.
4. B1 Vulkan benchmark run on the Vulkan build to pair with the B0 baseline now recorded.
5. clang-format pass + CI first green run; then M8 (one profiled Vulkan kernel behind a flag) and M10 soak/fuzz.
