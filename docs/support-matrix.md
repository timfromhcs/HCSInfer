# Support matrix (tested hardware, OS and models)

Version 1.1.0 line (1.1.0-rc.1 verified on the machine below; see the platform table for the other OSes). Status: **one machine, one OS verified end to end** — exactly what the evidence files show. This is not a compatibility guarantee; it is
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

## Per-platform support level (what ships, what is proven)

Levels: **Supported** = built by the release pipeline, required for a stable tag and exercised on real hardware by the
project. **CI-built** = built and portable-tested on a hosted runner, no real-hardware run by the project.
**Experimental** = built when the runner allows it, never blocks a release, no support promise. Hosted CI results only count
once a `release` run has actually executed on GitHub; until then the CI column is `NOT RUN`.

| Platform | Artifact | Level | What is proven | What is NOT proven |
|---|---|---|---|---|
| Windows 11 x64, CPU | `HCSInfer.exe`, portable ZIP, `win_amd64` wheel | **Supported** | full CTest with real models, real text / vision / SD1.5 / FLUX.3 runs, restricted-environment run (`final-s*`, `wave-b`, `wave-g`) | clean-machine run, code signing |
| Windows 11 x64, Vulkan | `HCSInfer-vulkan.exe` (needs the system `vulkan-1.dll`) | **Supported**, tested on Radeon 680M only | Vulkan CTest, benchmarks B0/B1, real models on the 680M | any other GPU or driver |
| Windows arm64 | none | not built | | everything |
| Linux x86_64, CPU | `HCSInfer-<ver>-linux-x86_64.tar.gz`, `manylinux_2_28_x86_64` wheel | **CI-built** | gcc 15 / clang 21 builds, portable ctest set and one real CPU generation in WSL2 (`final-v110/w1-posix`); release pipeline builds it in the manylinux_2_28 image with a glibc <= 2.28, NEEDED-library and no-RPATH gate | the hosted run (NOT RUN until executed), a native (non-WSL) Linux host, other distributions |
| Linux x86_64, Vulkan | `HCSInfer-vulkan` in the same archive when built | **Experimental** | the Vulkan loader seam was exercised by `doctor` against Mesa llvmpipe only | any GGML Vulkan inference on Linux, any real GPU (Mesa RADV or vendor), the SDK-built shader compiler inside the old-glibc image |
| Linux aarch64, CPU | `HCSInfer-<ver>-linux-aarch64.tar.gz`, `manylinux_2_28_aarch64` wheel | **Experimental** | nothing yet (hosted build NOT RUN) | everything |
| macOS arm64, CPU | `HCSInfer-<ver>-macos-arm64.tar.gz`, `macosx_11_0_arm64` wheel | **Experimental** | nothing yet (hosted build NOT RUN) | everything; no Metal backend exists |
| macOS x86_64 | none (the `macosx_10_15_x86_64` wheel tag exists in the wheel code, no binary is built) | not built | | everything |
| Any OS, pure `py3-none-any` wheel / sdist | Python client + launcher only | Supported as a client | `twine check`, install smoke | it contains no engine: needs `HCSINFER_EXE` |

Cross-platform feature limits: the coding-agent sidecar (`prime-agent` supervision) is **Windows only** (returns `Unsupported`
on Linux/macOS); all binaries are **unsigned**; PyPI upload is owner-gated and has not happened (`docs/pypi-publishing.md`).

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
