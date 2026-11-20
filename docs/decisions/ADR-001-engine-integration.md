# ADR-001: Engine integration and GGML topology

- Status: ACCEPTED for V1 development, scoped to SD1.5-class models (finalised 2026-11-17). Broader operator coverage remains an open evidence item.
- Date: 2026-11-17

## Context
HCSInfer reuses llama.cpp (text/vision) and stable-diffusion.cpp (diffusion). AGENTS.md 3.3 requires proving GGML compatibility before any shared-GGML or single-EXE claim.
Pinned: llama.cpp d3c3e242c16b8cbf51400fad410cd69b22d64f35 (own ggml/ tree), stable-diffusion.cpp 532d2c15c430a785a712b2d94281f04e733008a4 (master-1106-532d2c1) with patched ggml submodule leejet/ggml eeda9ec48fc84a3df0bb9dcc19ae9053075a2539.

## Options
1. Separate GGML per engine (two internal libraries/processes).
2. One shared upstream GGML (SD_USE_UPSTREAM_GGML=ON, SD_GGML_SOURCE_DIR=llama.cpp/ggml).
3. Fork/patch upstream GGML with the sd.cpp extensions.

## Evidence (all on Windows 11, MSVC 19.44, Ninja, CMake 4.4, Vulkan SDK 1.4.357.0, Radeon 680M, AMD proprietary driver)
Separate-build proof (AGENTS.md 17.2 steps 2-5):
- Patched GGML build (build/sd-vulkan): configure + build exit 0 (683 steps), sd-cli.exe/sd-server.exe produced.
- Upstream GGML build (build/sd-upstream-ggml, GGML source = external/llama.cpp/ggml @ d3c3e242): configure exit 0, build exit 0 (647 steps), sd-cli.exe produced. Only a CMake warning at configure: "INT8 tensorwise/convrot is disabled and FP8 weights are converted to F16 at load time. Some operators may be unsupported and performance may be lower than with patched GGML." No compile or link errors.
- Both builds enumerate Vulkan0 = AMD Radeon(TM) Graphics (uma 1, fp16 1, warp size 32, shared mem 32768, int dot 1, matrix cores none) and CPU.
- Runtime comparison with the real model <MODELS>\diffusion\sd15-q8_0.gguf (SD1.5 Q8_0, sha256 d0555243938c62faeefb4ac93f6c7a053ad373a4290c5256bce229aeb193bf94), 512x512, 15 steps, cfg 7, seed 42, Vulkan0 + params on CPU: output PNG is byte-identical between patched and upstream GGML (sha256 c31a5b04...1e7f). Wall time 38.3 s vs 38.9 s (single sample each, not a benchmark). Output visually verified as a red apple on wood.
- Same at 256x256/4 steps: patched-GGML and upstream-GGML PNGs byte-identical on the first run; a later repeat of the patched build gave a different PNG (hash differs) with identical settings, so Vulkan output is not guaranteed bit-reproducible run to run (cause not investigated; recorded as open observation).

Scope limits: only SD1.5 Q8_0 exercised. Operator coverage for DiT/Flux/Qwen-Image/Wan models, FP8/INT8 models and performance parity are NOT proven. The combined CMake graph (llama.cpp + sd.cpp in one project sharing one GGML target), duplicate-symbol checks and the single-EXE link are NOT RUN (llama.cpp baseline is another task).

## Additional evidence (2026-11-17, combined graph)
The HCSInfer CMake graph builds `hcs_engine_llama` and `hcs_engine_diffusion` into one `HCSInfer.exe` with one GGML (llama.cpp's, `SD_USE_UPSTREAM_GGML=ON`, `SD_GGML_SOURCE_DIR=external/llama.cpp/ggml`; options in `external/dependency-lock.json`). Real runs through that binary: llama text on CPU and Vulkan (`docs/test-evidence/M2/llama-engine-integration.txt`) and SD1.5 Q8_0 text-to-image/img2img on CPU and Vulkan (`docs/test-evidence/M2/image/README.md`). No duplicate-symbol or link errors were observed. Still NOT RUN: Flux/DiT/Qwen-Image or FP8/INT8 models, performance parity against patched GGML, a clean-machine test.

## Decision
Adopt option 2 (one shared upstream GGML) for V1: it builds, links in one process and matches patched output for SD1.5. Engine adapters stay isolated so option 1 (separate internal libs or dev multi-process) remains a fallback if a needed diffusion model family fails on upstream GGML. A pin bump of either submodule requires re-running the evidence above. The single-EXE claim stays BLOCKED until a static-CRT build and a clean-machine test pass (ADR-008) and broader model coverage is compared. Do not alter either upstream tree.

## Rejected (so far)
- Silently linking two different GGML versions in one process.
- Forking GGML before a measured need exists.

## Risks
Loss of patched operators/FP8/INT8 paths for newer diffusion models; upstream GGML API drift on each pin bump; ggml version skew between llama.cpp and sd.cpp private headers.

## Migration / next evidence
Run a Flux/Qwen-Image-class model in both modes once a supported GGUF is available; record operator coverage; revisit this ADR if upstream GGML cannot run a required model family.
