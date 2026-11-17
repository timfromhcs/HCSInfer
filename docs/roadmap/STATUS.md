# HCSInfer status

Last updated: 2026-11-17 (documentation/CI/packaging pass, based on inspection of the working tree and
`docs/test-evidence/`). Statuses: PASS / FAIL / BLOCKED / NOT RUN. Nothing below is claimed without an evidence
file or an inspected source file; items with no evidence are NOT RUN or BLOCKED.

## Repository state

- Git repo on `main` with **no commits yet**. Index contains only `.gitmodules` and the two submodule pins
  (`external/llama.cpp` d3c3e242c16b8cbf51400fad410cd69b22d64f35, `external/stable-diffusion.cpp`
  532d2c15c430a785a712b2d94281f04e733008a4); everything else is untracked. No other branches exist, so there is
  no branch diff to report.
- Toolchain on the target machine: MSVC 19.44, CMake 4.4.0, Ninja 1.13.2, Vulkan SDK 1.4.357.0 (build only).
- Modules present in `src/`: `core`, `hardware`, `storage`, `engines/llama`, `engines/diffusion`, `api`, `mcp`,
  `cli`, `app`. **Not present:** `gui`, `memory`, `scheduler`, `telemetry`, `kernels`, `shaders`.
- Stray files at the repository root (`cpu-256x256-2step.png`, `cpu-edit-256x256-2step.png`) are smoke-test
  outputs and should be deleted or ignored by their owner; they are not part of the product.

## Gate table

| Gate | Status | Evidence / reason |
|---|---|---|
| Baseline test run (Release, `windows-msvc-release`) | PASS | 294 tests: 281 pass, 13 skipped because `HCS_TEST_GGUF` is unset (skips print a NOT RUN message) |
| Same suite with `HCS_TEST_GGUF` set | PASS (CPU real tests, `e2e_api_mcp_real`) | as reported by the build owner for this pass; Vulkan real tests are skipped in the non-Vulkan preset (NOT RUN there) |
| Configure + build Debug / Release (MSVC x64, Ninja, `/W4 /WX` on HCS targets) | PASS | `docs/test-evidence/M2-core/`, `M3-api-mcp/ctest-debug.txt`; preset `windows-msvc-release-vulkan` configures and builds (`M2/image/README.md`) |
| M0 hardware doctor (CPU features, RAM, OS, Vulkan devices/heaps, GPU adapters, drive info, read bench) | PASS | `docs/test-evidence/M0/` (`doctor.json`, `doctor-no-vulkan.json`, `readbench-usb-hdd.json`, `vulkaninfo-summary.txt`, ctest logs) |
| M1 llama.cpp independent baseline, CPU + Vulkan, real GGUF from USB drive | PASS | `M1/llama/README.md`; qwen3-0.6b Q4_K_M, sha256 4ea79412...; llama-bench CPU vs Vulkan is provisional (not a controlled benchmark) |
| M1 stable-diffusion.cpp baseline, patched vs `SD_USE_UPSTREAM_GGML=ON`, CPU + Vulkan | PASS (SD1.5 Q8_0 only) | `M1/sd/README.md`; 512x512 output byte-identical between GGML modes once; model licence unverified |
| M1 multimodal (libmtmd) upstream proof | PARTIAL: NOT RUN as a standalone `llama-mtmd-cli` run; the same pinned libmtmd is proven through the HCS adapter | real SmolVLM2-500M-Video-Instruct Q8_0 + mmproj answer correctly on CPU and Vulkan (`M2-vision/`); standalone upstream CLI not run |
| M2 headless core (lifecycle, registry, JobQueue, MemoryBudget, PathGuard, settings) | PASS | unit tests; `M2-core/` |
| M2 control-plane facade (`src/api/control_plane.cpp`) loads text or image models, switches, frees previous model | PASS (text, real); image path exercised via CLI only | `M2/llama-adapter-README.md`, `M2/image/README.md`; full facade-level image regression is NOT RUN |
| M2 llama adapter (text), CPU + Vulkan, cancellation, 10x load/unload | PASS | `M2/llama-engine-integration.txt` (16/16 on a quiet machine; a first run under heavy concurrent load failed 9 tests with host OOM, documented in the README) |
| M2 llama adapter vision (`analyze_image`) | PASS (SmolVLM2-500M Q8_0 only; CPU and Vulkan) | `M2-vision/README.md`: 10/10 real-model engine tests on the Vulkan build (9 + 1 NOT RUN on the CPU build), bad-image / mismatched-mmproj / cancel / reload tests |
| M2 diffusion adapter text-to-image + img2img, CPU + Vulkan, via `HCSInfer image` | PASS | `M2/image/README.md` (SD1.5 Q8_0; single samples, not benchmarks) |
| CLI | PASS for `doctor`, `models scan/list/inspect/load/unload`, `chat`, `vision`, `image generate/edit`, `serve`, `mcp stdio` | `tests/contract/cli`, `M2-vision/`; `benchmark`, `tune`, `cache` return exit code 10 (planned, `src/cli/cli.cpp`) |
| M3 HTTP API: `/v1/models`, `/v1/chat/completions` (+streaming), `/hcs/v1/{health,runtime,devices,memory,models,jobs}`, auth, loopback, limits | PASS (experimental) | `tests/contract/api*`, `M3-api-mcp/` (`e2e_api_mcp_real`) |
| HTTP image routes (`/v1/images/generations`, `/v1/images/edits`), `POST /hcs/v1/benchmarks` | FAIL / not implemented (vision input on chat is implemented, next row) | return 501 `unsupported` |
| HTTP vision input on `/v1/chat/completions` (`image_url` data URI, streaming, cancel) | PASS (experimental; SmolVLM2 only; CPU + Vulkan builds) | `M2-vision/` (`e2e_vision.py`: 27/27 each), `tests/contract/api_vision` |
| M3 MCP stdio (models, chat, runtime, jobs tools; path guard) | PASS (experimental) | `tests/contract/mcp`, `e2e_real.py` |
| MCP `hcs.vision.analyze` | PASS (experimental; real VLM over stdio, CPU + Vulkan builds) | `M2-vision/`, `tests/contract/api_vision` |
| MCP `hcs.image.generate`, `hcs.image.edit` | FAIL / not implemented | tools exist, return `unsupported` |
| MCP Streamable HTTP | NOT RUN (intentionally not implemented) | `docs/mcp.md` |
| M4 GUI | NOT RUN | no `src/gui`; ADR-009 proposes an embedded local web UI served by the loopback API (proposal, not built) |
| M5 Model Vault v1 (bounded async read pipeline, HDD source, no SSD staging) | PARTIAL | `src/storage` (`file_source`, `stream`, `model_reader`) tested read-only on a real GGUF; engine loading does **not** yet go through the vault; USB-unplug test, cold e2e and engine integration are NOT RUN |
| M6 HCSMODEL container (header, chunk table, Raw/LZ4/Zstd, hashes, random access) | PASS (experimental) / PARTIAL for fuzz | 59 unit + 3 parser + 2 integration tests; libFuzzer smoke only 60 s with UBSan, no ASan (`M6-storage/`). Throughput benefit on a real HDD is NOT proven: measured raw HDD read 65 MiB/s is recorded, compression gain is only what the bench JSON shows |
| M7 shared GGML ABI (one GGML in one process: llama.cpp GGML + sd.cpp via `SD_USE_UPSTREAM_GGML=ON`) | PASS for build + SD1.5 + llama text in one binary | `HCSInfer.exe` links both; coverage for Flux/DiT/Qwen-Image, FP8/INT8 models is NOT RUN |
| M7 single `HCSInfer.exe`, static CRT, embedded assets, clean-machine test | NOT RUN | CRT is the default dynamic `/MD`; no clean-machine test. Packaging is a portable folder only (ADR-008) |
| M8 HCS Vulkan kernels | NOT RUN | no `src/kernels`, no `shaders/` |
| M9 research features | NOT RUN | |
| M10 security audit, 100-cycle soak, 30-min run, OOM/device-lost/HDD-unplug, signing | NOT RUN | only 10x load/unload and the cancellation tests exist |
| Real Radeon 680M test | PASS for llama text, diffusion, doctor | evidence above; kernel tests NOT RUN |
| Benchmarks (AGENTS.md 21 methodology) | PARTIAL | M1 numbers are provisional/uncontrolled; storage bench JSON follows schema v1 but with `cold_or_warm` uncontrolled; `HCSInfer benchmark` is not implemented |
| clang-format check | NOT RUN locally | `scripts/format.ps1` added; no formatting pass has been applied yet |
| clang-tidy, shader/SPIR-V validation | NOT RUN / n.a. | no shaders exist |
| CI workflows | CREATED, NOT RUN | `.github/workflows/pr.yml`, `hardware.yml`; the repo has no commits or remote, so they have never executed |
| SBOM / checksums / package | CREATED, NOT RUN in CI | `scripts/sbom.ps1`, `scripts/package.ps1` (portable folder, not single-EXE) |
| ADR-001..010 | ADR-005/006/007 accepted earlier; 001-004, 008-010 written/finalised this pass | `docs/decisions/` |

## Known limitations

- HTTP/MCP image generation/editing routes are unimplemented; vision is limited to one image per single-turn request and one tested model family (SmolVLM2).
- No GUI. No benchmark command. No kernels. No scheduler beyond the single-worker JobQueue.
- Model loading by the engines reads the file itself (mmap/engine loader); the HCS Model Vault pipeline is not in
  the load path yet.
- Cold SD1.5 first step took about 218 s when tensors streamed at 5-13 MB/s (`M2/image/README.md`); warm runs are
  much faster. Cold and warm numbers must not be compared.
- Vulkan SD output was once not byte-reproducible run to run (not investigated).
- 13 tests skip without `HCS_TEST_GGUF`; they are NOT RUN, not PASS, in a CI without models.

## Exact next action

1. Vision follow-ups: more VLM families (Qwen-VL M-RoPE, Gemma3 non-causal), projector memory accounting, standalone upstream `llama-mtmd-cli` baseline, multi-turn image chat.
2. Wire `/v1/images/*`, MCP image tools and chat image input to the existing engines.
3. Route engine model reads through the Model Vault, then M5 cold/warm benchmarks.
4. GUI per ADR-009; `HCSInfer benchmark`; first commit so CI can run.
