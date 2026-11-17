# HCSInfer status

Last updated: 2026-11-17 (after V1.1 wave 1 and the wave-2 browser demo `web/`). Statuses: PASS / FAIL / BLOCKED /
NOT RUN. Nothing below is claimed without an evidence file or an inspected source file; items with no evidence are
NOT RUN or BLOCKED.

## Repository state

- Git repo on branch `main`, pushed to the **public** remote `https://github.com/timfromhcs/HCSInfer` (created
  2026-11-17). Local and remote `main` are in sync; `main` is the only branch, so there is still no branch-to-branch
  diff. The history starts at `d5baf76` "Initial commit: HCSInfer core, engines, API, MCP, vault, vision, docs"
  (2026-11-17 13:57 +0100) and the work described below was committed on top of it in four further commits.
- Submodule pins: `external/llama.cpp` d3c3e242c16b8cbf51400fad410cd69b22d64f35, `external/stable-diffusion.cpp`
  532d2c15c430a785a712b2d94281f04e733008a4, `external/prime-agent` 8980e66f2f07762d62a6c1d73b347a4cac5275cc
  (see `external/dependency-lock.json`).
- Toolchain on the target machine: MSVC 19.44.35228 (toolset 14.44.35207) x64, CMake 4.4.0, Ninja 1.13.2,
  Vulkan SDK 1.4.357.0 (build time only), Windows 11 build 10.0.26200.9974, Ryzen 7 7735HS, Radeon 680M
  (device id 0x1681), ~21.2 GB RAM, AMD driver 32.0.21043.12001.
- Modules present in `src/`: `core`, `hardware`, `storage`, `agent`, `train`, `engines/llama`,
  `engines/diffusion`, `gui`, `api`, `mcp`, `cli`, `app`. **Still absent:** `memory`, `scheduler`, `telemetry`,
  `kernels`, `shaders`. A separate browser deliverable `web/` (HCS Studio Web, ADR-013) exists outside `src/` and
  outside the CMake graph.
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
| B7 compute backend vs parameter backend (sdcpp `--backend` / `--params-backend`) | PASS (measured, one sample each) | `B7-params/`: vulkan0+cpu-params 5.22 s / 1773 MB peak WS, vulkan0+disk-params 3.29 s / 625 MB, cpu+cpu 9.68 s; outputs differ between placements (equivalence NOT verified); the model is on internal NVMe, so a USB-HDD disk-params case is NOT RUN |
| M1 multimodal (libmtmd) upstream proof | PARTIAL: NOT RUN as a standalone `llama-mtmd-cli` run; the pinned libmtmd is proven through the HCS adapter | `M2-vision/` (SmolVLM2-500M Q8_0 + mmproj, CPU and Vulkan) |
| M2 headless core (lifecycle, registry, JobQueue, MemoryBudget, PathGuard, settings) | PASS | unit tests; `M2-core/` |
| M2 control-plane facade: text or image models, switching, freeing the previous model | PASS (text and image, real) | `M2/llama-adapter-README.md`, `M2/image/README.md`, `M3-image/` |
| M2 llama adapter (text), CPU + Vulkan, cancellation, 10x load/unload | PASS | `M2/llama-engine-integration.txt` (16/16) |
| M2 llama adapter vision (`analyze_image`) | PASS (SmolVLM2-500M Q8_0 only; CPU and Vulkan) | `M2-vision/README.md`: 10/10 real-model engine tests on the Vulkan build including bad image / mismatched mmproj / cancel / reload |
| M2 diffusion adapter text-to-image + img2img, CPU + Vulkan | PASS | `M2/image/README.md`; integration tests `DiffusionBlocking.*` (6/6) now run with `HCS_TEST_DIFFUSION_MODEL` |
| CLI | PASS for `doctor`, `models scan/list/inspect/load/unload`, `chat`, `vision`, `image generate/edit`, `serve`, `mcp stdio`, `benchmark` | `tests/contract/cli`, `M2-vision/`, `benchmark/`; only `tune`, `cache` still return exit code 10 (planned) |
| `HCSInfer benchmark --suite quick` (real text benchmark, AGENTS.md 21.1 JSON) | PASS (one real CPU run) | `benchmark/`: B0-text, CPU, qwen3-0.6b Q4_K_M, cold+warm phases, full metadata, sha256 recorded. `--suite full` is NOT RUN (not implemented) |
| M3 HTTP API: `/v1/models`, `/v1/chat/completions` (+streaming), `/hcs/v1/{health,runtime,devices,memory,models,jobs}`, auth, loopback, limits | PASS (experimental) | `tests/contract/api*`, `M3-api-mcp/`, `M10-fixes/e2e-api-mcp-real-after-fix.json` |
| M3 HTTP image routes `/v1/images/generations`, `/v1/images/edits` | **PASS (experimental)** | `M3-image/`: 47/47 checks over a real socket with SD1.5 Q8_0 on the **CPU build and the Vulkan build** (`vulkan-e2e-image.json`, plus the four `vulkan-*.png` renders) - real PNG of the requested size (not blank), edit keeps the input size, validation/limits, `multipart` -> 415, abandoned request cancels the render |
| HTTP vision input on `/v1/chat/completions` (`image_url` data URI, streaming, cancel) | PASS (experimental; SmolVLM2 only) | `M2-vision/` (`e2e_vision.py` 27/27), `tests/contract/api_vision`, `e2e_vision_real` |
| M3 MCP stdio (models, chat, runtime, jobs tools; path guard) | PASS (experimental) | `tests/contract/mcp`, `M10-fixes/e2e-api-mcp-real-after-fix.json` |
| MCP `hcs.vision.analyze` | PASS (experimental; real VLM over stdio) | `M2-vision/`, `tests/contract/api_vision` |
| MCP `hcs.image.generate`, `hcs.image.edit` | **PASS (experimental)** | `M3-image/`: real render, image returned as an MCP image content block, size kept, approved-root enforcement, cancellation |
| `POST /hcs/v1/benchmarks` | FAIL / not implemented | returns `501`; use the CLI `benchmark` command |
| MCP Streamable HTTP | NOT RUN (intentionally not implemented) | `docs/mcp.md` |
| M4 GUI (embedded local web UI, ADR-009 accepted) | **PASS (experimental)** for the HTTP/browser-boot phases, on both builds | `M4-gui/`: 23 checks 0 failed over a real socket (CSP, traversal, Host/Origin, 405, token not embedded) **plus a real headless-Chrome boot** that connects with the fragment token and lists the models; the same script against the **Vulkan build** is `vulkan-e2e-gui.json` (22 checks, 0 failed - one check needs a registered model). Python-`playwright` interaction phase NOT RUN (playwright not installed) |
| M4 normal-user workflow without a terminal | PARTIAL | manual run recorded in `M4-gui/README.md` (load, streamed chat, Stop/cancel, vision, Performance); not automated |
| V1.1 wave 1: LFM3-Omni tool calls + agentic eval, FLUX.3 Klein plumbing + presets, HCS Studio UI, agent/train foundations | PASS (experimental) | `docs/test-evidence/lfm3`, `flux3`, `agent`, `studio`; the FLUX.3 render stays BLOCKED (gated encoder/VAE). Commit `6a390b8` (V1.1 wave 1) |
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
| Text benchmark baselines | PASS (B0 CPU + B1 Vulkan, one controlled run each) | `benchmark/`: same model/prompt/parameters/commit, qwen3-0.6b Q4_K_M, cold+warm phases, full AGENTS.md 21.1 metadata. Other prefill/decode suites NOT RUN; `--suite full` not implemented |
| B0/B1 + M5 load-mode matrix (B4/B8 partial) | PASS (measured) | `M5-loadmodes/`: `auto`, `mmap`, `none`, `mlock`, `mmap+mlock`, `direct_io` on the real model from the USB drive, each with peak working set/commit; `--load-mode` is now exposed by `benchmark` and `vision` too (contract test) |
| B2/B5/B6 (best placement reference, LZ4/Zstd chunk loading) | NOT RUN | need the Model Vault in the load path (M5 Stage B) or a further implementation; B7 (diffusion compute vs parameter backend) is now measured in `B7-params/` |
| clang-format / clang-tidy | **PASS** (after fixing a wave-1 regression) | `scripts/format.ps1` green over 152 files with clang-format 19.1.5 (VS-bundled, pinned); `scripts/static-analysis.ps1 -Scope src -FailOnFindings` reports **0 diagnostics in 51 translation units**. **V1.1 wave 1 (`6a390b8`) broke both gates on `main`** (CI run 44154743021): 4 files needed formatting and clang-tidy 20.1.8 found 6 `bugprone-implicit-widening-of-multiplication-result` sites in `include/hcs/{agent,api,train}` plus 1 `clang-analyzer` location inside `external/.../stb_image.h`. Fixed: widened the size constants first, and `static-analysis.ps1` now ignores diagnostics located under `external/` (the gate covers HCS-owned code). Evidence `docs/test-evidence/static-analysis/wave1-*`; triage in `docs/static-analysis.md` |
| Shader / SPIR-V validation | n.a. / NOT RUN | no shaders exist |
| CI workflows | **PASS (green again on `main`)** | `.github/workflows/pr.yml` run 44167798984 (2026-11-17, commit `d8a5e37`): all six jobs green - `build + ctest` Release (18m38s) and Debug (9m59s), `clang-tidy` (5m53s), `clang-format` (17s), `SBOM` (17s), and the new `web` job (26s, 46 unit + static tests and the blocking headless boot check). First green reference: run 44105659773 (commit `f9cd395`), all four then-existing jobs green - `clang-format check` (16 s), `SBOM generation + schema sanity` (11 s), `build + ctest (windows-msvc-release)` (16 m 45 s), `build + ctest (windows-msvc-debug)` (8 m 21 s). The two earlier runs failed: run 44104062457 in the build legs (Visual Studio not located on the runner, fixed in `fddc30d`) and run 44104849967 was cancelled by the next push. Model-dependent tests skip there (NOT RUN), so this proves configure/build/unit/contract/parser on a clean hosted machine, not Vulkan or real-model behaviour. **V1.1 wave 1 (`6a390b8`, run 44154743021) failed the `clang-format` and `clang-tidy` jobs** (build/SBOM stayed green) - fixed in commits `06cfbe4` (format + tidy) and `8b29907` (web probe timeout); the new `web` job was added and made fully blocking in `d8a5e37` |
| SBOM / checksums / package | PASS (portable folder + ZIP) / single-EXE NOT RUN | `scripts/package.ps1 -Preset img-api -Zip` produced `HCSInfer-0.1.0-img-api-portable` (21 files, 12 licence texts, manifest, CycloneDX SBOM with 13 components, SHA256SUMS over 20 files) and a 17.2 MiB ZIP with SHA-256 `527df693...`; the packaged `HCSInfer.exe` runs standalone (`doctor --json`, exit 0) - `M10-fixes/../package/`. Still open: clean-machine test without the MSVC redistributable, signing, single-EXE |
| ADR-001..010 | ADR-005/006/007/009 accepted; 001-004, 008, 010 written/finalised | `docs/decisions/` |

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
- There is no remote, so nothing is backed up and CI has never run.

## Exact next action

1. **V1.1 wave 2 leftovers**: the HCS Agent adapter (supervise `prime-agent` as a sidecar over the loopback API
   with the `hcs::agent` sandbox policy, ADR-011) and the HCS Train LoRA pipeline + eval gate (ADR-012). Wave 1 is
   committed (`6a390b8`); wave 2's first item, the browser demo `web/`, is done (ADR-013).
2. **`web/` follow-ups**: run the demo in a real WebGPU browser (real inference is NOT RUN here - headless has no
   adapter) and deploy to Vercel (NOT RUN, needs the owner's credentials).
3. **M5**: route engine model reads through the Model Vault, then cold/warm and the USB-unplug test - the critical
   path to B4/B5/B6/B8. Blocked on this machine (no model larger than RAM, no admin rights to flush the file cache).
4. **M8 / M10**: profile the upstream Vulkan backend and land one kernel behind a feature flag (quantised GEMV
   first); then soak (100 load/unload, 30-min generation), OOM/device-lost/HDD-unplug injection, and the single-EXE
   static-CRT build (ADR-008).
5. **Small V1 gaps**: `POST /hcs/v1/benchmarks` (returns `501` today) and the `tune`/`cache` CLI commands.
