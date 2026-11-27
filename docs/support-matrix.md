# Support matrix (tested hardware, OS and models)

Version 1.1.0-rc.1. Status: **one machine, one OS** — exactly what the evidence files show. This is not a compatibility guarantee; it is
the list of configurations HCSInfer has actually been built and run on, with the status of each. AGENTS.md 20:
general RDNA/Radeon or Windows claims are **not** made from one device.

Test machine (target hardware, AGENTS.md 1.3):

| Component | Value | Source |
|---|---|---|
| CPU | AMD Ryzen 7 7735HS, 8 cores / 16 threads, Zen 3+, AVX2 + FMA3 | `docs/test-evidence/M0/doctor.json` |
| RAM | ~21.2 GB (21,245,952,000 bytes) shared with the iGPU | `doctor.json`, benchmark JSON |
| GPU | AMD Radeon(TM) Graphics (Radeon 680M), vendor `0x1002`, device `0x1681`, integrated GPU | `doctor.json`, `M0/vulkaninfo-summary.txt` |
| Graphics driver | AMD `32.0.21043.12001`; Vulkan API `1.4.315`; shader compiler `26.6.1` | `doctor.json` |
| Vulkan device features | UMA `1`, fp16 `1`, subgroup/warp size `32` | `M0/vulkaninfo-summary.txt` |
| OS | Windows 11, build `10.0.26200.9974` | `doctor.json` |
| Toolchain | MSVC 19.44.35228 (VS 2022), CMake 4.4.0, Ninja 1.13.2, Vulkan SDK 1.4.357.0 (build time only) | `docs/build.md` |

## Backends

| Backend | Status | Evidence |
|---|---|---|
| GGML CPU (text, vision, diffusion) | PASS | `M1/llama`, `M2-vision`, `M1/sd`, `benchmark/` |
| GGML Vulkan — text | PASS | `benchmark/` (B1), `M2/`, `lfm3/` |
| GGML Vulkan — vision (libmtmd) | PASS | `M2-vision/` |
| GGML Vulkan — diffusion (SD1.5) | PASS | `M3-image/` (Vulkan build), `B7-params/` |
| HCS Vulkan kernel (Q8_0 GEMV, feature-flagged OFF) | PASS for correctness; not default-enabled | `vulkan-kernels/` |
| CPU-only fallback (Vulkan unavailable) | PASS | `M0/doctor-no-vulkan.json`, CPU runs of text/vision/diffusion |
| Linux x86-64 CPU build (gcc, clang) | PASS (WSL2 Ubuntu, one real LFM3 generation) | `final-v110/w1-posix/` |
| Linux / Mesa RADV (real AMD GPU) | **NOT RUN** | the Vulkan loader seam was exercised against Mesa llvmpipe by `doctor` only; no GGML Vulkan build (no `glslc` in the WSL image) and no AMD GPU run |
| macOS arm64 | **NOT RUN** | code paths exist, never built |
| ROCm | **NOT RUN** (explicitly out of V1 scope, AGENTS.md 1.2) | — |
| Other AMD GPUs / discrete GPUs / NVIDIA / Intel | **NOT RUN** | one device only |

## Models actually run (weights never distributed)

| Model | Kind | Backend | Status | Evidence |
|---|---|---|---|---|
| qwen3-0.6B Q4_K_M | text | CPU + Vulkan | PASS | `benchmark/`, `M1/llama` |
| LiquidAI LFM3-Omni-2B Q8_0 (+ mmproj) | text, tool calls, vision | CPU + Vulkan | PASS | `lfm3/` |
| SmolVLM2-500M-Video-Instruct Q8_0 (+ mmproj) | vision | CPU + Vulkan | PASS | `M2-vision/` |
| SD 1.5 Q8_0 | diffusion (t2i, i2i) | CPU + Vulkan | PASS | `M1/sd`, `M3-image/` |
| FLUX.3 Image Klein (unsloth UD-Q4_K_XL, substitute Qwen3-VL-4B Q4_K_M encoder + FLUX.2 VAE) | diffusion (t2i, i2i) | Vulkan: t2i + i2i; CPU: t2i only (n=1), no CPU i2i | PASS (single samples, 364-471 s per 512x512 on the 680M; the gated official BFL encoder/VAE files were not accessible) | `wave-f/` |

## Feature status by area

| Area | Status | Notes |
|---|---|---|
| Chat, HTTP API, MCP stdio | PASS | `M3-api-mcp/`, `mcp-benchmark/` |
| Vision (one image, single turn) | PASS | SmolVLM2 + LFM3-VL only |
| Image generation / editing | PASS | SD1.5 and FLUX.3 Klein (slow: minutes per image; FLUX.3 image-to-image on Vulkan only, CPU text-to-image only, n=1); `url`/`mask`/multipart rejected by design |
| Embedded web UI (HCS Studio) | PASS (HTTP + real headless-browser interaction, CPU and Vulkan builds) | `wave-a/gui-cpu`, `wave-a/gui-vulkan`, `wave-c/`; a human-driven session on a second machine NOT RUN |
| Model Vault (bounded reads) | PARTIAL | engines do not load through it; HDD-bound case BLOCKED |
| HCSMODEL container | PASS (experimental) | no measured HDD benefit |
| Static `HCSInfer.exe` (CPU, system-DLL-only imports) and `HCSInfer-vulkan.exe` (also imports the system `vulkan-1.dll`); not called single-file | system-DLL-only imports (`dumpbin`) and restricted-environment run PASS; **clean machine NOT RUN**; unsigned | `wave-b/`, `wave-g/restricted-env-static-exe.json` |
| Python wheel / sdist `hcsinfer` | PASS locally (fresh venv, real chat); not on PyPI | `wave-b/` |
| Training / agent sidecar / web demo | logic-tested only | real runs NOT RUN (`train/`, `agent/`, `web/`) |

## How to extend this matrix

A new row is only added with an evidence file under `docs/test-evidence/` that records the exact device id, driver,
OS build, Vulkan version and model hash, following AGENTS.md 21.1. Untested combinations are listed as NOT RUN, never
as PASS by association.
