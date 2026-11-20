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
* `HCSInfer benchmark --suite quick/full` regression in the default build: **790/790 CTest PASS**; the
  `windows-msvc-release-kernels` preset runs the whole suite as **794/794** (adds the 4 `kernel-vulkan` tests),
  and `ctest -L "kernel-cpu|kernel-vulkan"` is **10/10 PASS**.

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

## Kernel variant A/B (v1 Scalar vs v2 Tiled) — hypothesis rejected

`gemv-q8_0-variants-bench.json`, `--reps 30`, same device. v2 stages the activation vector `x` in workgroup shared
memory per K-tile to cut x traffic; the hypothesis was that this was the bottleneck.

| m x k | CPU ms | v1 ms | v1 GiB/s | v2 ms | v2 GiB/s |
|---|---|---|---|---|---|
| 512x512 | 0.179 | 1.285 | 0.20 | 1.292 | 0.20 |
| 1024x1024 | 0.704 | 1.474 | 0.70 | 1.390 | 0.75 |
| 2048x2048 | 2.809 | 1.942 | 2.14 | 1.980 | 2.10 |
| 4096x4096 | 11.391 | 4.800 | 3.46 | 4.630 | 3.59 |
| 4096x11008 | 30.416 | 12.589 | 3.54 | 13.203 | 3.38 |
| 11008x4096 | 30.891 | 14.356 | 3.11 | 14.342 | 3.11 |

**Decision:** v2 is the same as v1 within noise, so the hypothesis is **rejected**; v1 (`Variant::Scalar`) stays the
default and v2 stays behind the flag for reproducibility. Full write-up:
`docs/research/vulkan-gemv-v2-shared-memory.md`.

## Wave K (2026-11-20): upstream baseline, validation layers, fallback

See `m8-baseline/README.md`. Upstream Vulkan Q8_0 `mul_mat_vec` 4096x14336 n=1: 0.822 ms; HCS v1: 14.99 ms (18.2x slower)
-> **stays OFF**. Validation layers found and fixed a SPIR-V 1.5 vs Vulkan 1.1 instance mismatch (now `vulkan1.1`, 0
messages). Device-lost and flag-off fallback tests added (12/12 `kernel-cpu`+`kernel-vulkan` PASS).

## NOT RUN / open

* A **real** (driver-provoked) device-lost experiment; **subgroup/fp16/int8** feature use. (Upstream baseline and
  validation layers: DONE in wave K.)
* A **tuned** version (staging buffers, shared-memory reduction, multiple rows per workgroup) and the
  kernel-registry/autotuner cache.
* Any **default-enable** decision: it requires a repeatable measured benefit over the selected fallback on the
  supported device, which does not exist yet.
