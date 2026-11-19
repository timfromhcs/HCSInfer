# Evidence: HCS Vulkan kernel — Q8_0 GEMV (M8)

Status: **PASS for correctness, PARTIAL for performance (not default-enabled).** One kernel exists, is
feature-flagged and off, and is not on the model load path.

## What was run

Build: `windows-msvc-release-kernels` (`-DHCS_ENABLE_VULKAN_KERNELS=ON`), MSVC 19.44.35228, CMake 4.4.0,
Ninja 1.13.2, Vulkan SDK 1.4.357.0 (build time). Target machine, 2026-11-19.

* The shader `shaders/llm/gemv_q8_0.comp` is compiled with `glslc --target-env=vulkan1.2`, validated with
  `spirv-val --target-env vulkan1.2`, and embedded as SPIR-V (the build step `Compiling + validating shader
  GemvQ8_0` is part of the target build).
* `tests/unit/kernels` (`kernel-cpu`): **6/6 PASS** — fp16 conversion (round-trip, subnormal, `-0`, Inf),
  Q8_0 sizing, and the reference against an independent dequantise-and-dot loop.
* `tests/integration/kernels_vulkan` (`kernel-vulkan`): **4/4 PASS** on the real device
  `AMD Radeon(TM) Graphics` (integrated_gpu) — CPU-vs-Vulkan match over shapes 1x32, 7x160, 64x256, 129x512,
  512x1024 (max abs err ~1e-5), the feature flag default-off path, shape validation, and determinism over 20
  repeated calls.
* `HCSInfer benchmark --suite quick/full` regression in the default build: **790/790 CTest PASS**.

## Benchmark (isolated kernel, `hcs_kernel_bench --reps 20 --device Radeon`)

`gemv-q8_0-bench.json`. Median of 20 timed calls after warm-up; CPU is the single-threaded reference.

| m | k | CPU (ms) | Vulkan (ms) | CPU/Vulkan | max abs err | Vulkan GiB/s |
|---|---|---|---|---|---|---|
| 512 | 512 | 0.171 | 1.313 | 0.1x | 4.8e-06 | 0.20 |
| 1024 | 1024 | 0.679 | 1.408 | 0.5x | 1.1e-05 | 0.74 |
| 2048 | 2048 | 2.761 | 1.910 | 1.4x | 1.9e-05 | 2.17 |
| 4096 | 4096 | 11.11 | 4.558 | 2.4x | 3.5e-05 | 3.64 |
| 4096 | 11008 | 30.01 | 12.01 | 2.5x | 8.4e-05 | 3.71 |
| 11008 | 4096 | 30.00 | 14.31 | 2.1x | 3.8e-05 | 3.12 |

**Honest reading:** the kernel is correct, and on large shapes it beats the *single-threaded CPU reference*
by ~2.1-2.5x at ~3.6 GiB/s of Q8_0 weight traffic. On small shapes Vulkan is slower (per-call buffer
allocation + dispatch overhead). Effective bandwidth (~3.7 GiB/s) is far below the 680M's shared-memory
bandwidth, so the kernel is **not** default-enabled — v1 is correctness-first (host-visible buffers, no
staging, one thread per row), and the baseline that matters (GGML's own Vulkan GEMV) has not been compared yet.

## Reproduce

```powershell
$env:VULKAN_SDK = "C:\VulkanSDK\1.4.357.0"
.\scripts\build.ps1 -Preset windows-msvc-release-kernels
build\windows-msvc-release-kernels\bin\hcs_test_kernels.exe --gtest_brief=1
build\windows-msvc-release-kernels\bin\hcs_test_kernels_vulkan.exe
build\windows-msvc-release-kernels\bin\hcs_kernel_bench.exe --reps 20 --device Radeon
```

## NOT RUN / open

* **Upstream-Vulkan baseline** (GGML's Vulkan GEMV) — the kernel is not wired into GGML's backend.
* Vulkan **validation layers** on the kernel; a **device-lost** experiment; **subgroup/fp16/int8** feature use.
* A **tuned** version (staging buffers, shared-memory reduction, multiple rows per workgroup) and the
  kernel-registry/autotuner cache.
* Any **default-enable** decision: it requires a repeatable measured benefit over the selected fallback on the
  supported device, which does not exist yet.
