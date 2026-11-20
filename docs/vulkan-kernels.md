# HCS Vulkan kernels (M8)

Status: **Experimental, feature-flagged and OFF by default.** One kernel exists today: a **Q8_0 GEMV**
(`y[m] = sum_k dequant_q8_0(W[m,k]) * x[k]`). It is **not on the model load path** — the engines keep using
GGML's own CPU/Vulkan operators. This page documents what is built, how it is validated, and the exact limits.

Related: `docs/decisions/ADR-003-vulkan-ownership.md`, AGENTS.md 12 (kernel engineering) and 15 (research rules).

## Why it exists

AGENTS.md M8 says: profile upstream first, then land **one** candidate kernel behind a feature flag, with an
independent CPU reference, SPIR-V validation, a benchmark against a baseline and a fallback. This module is
that first candidate. It is deliberately small and self-contained so the whole toolchain (GLSL -> SPIR-V ->
Vulkan dispatch -> readback) is proven end to end on the target machine before any operator is default-enabled.

## Layout

```
shaders/llm/gemv_q8_0.comp          GLSL compute shader (author: HCSInfer; not derived from third-party code)
include/hcs/kernels/cpu.hpp         CPU reference API + Q8_0 sizing + fp16 conversion
include/hcs/kernels/vulkan.hpp      feature flag, device query, gemv_q8_0()
src/kernels/cpu/gemv_q8_0.cpp       reference implementation (the oracle + fallback)
src/kernels/vulkan/gemv_q8_0.cpp    dynamic Vulkan loader, pipeline, dispatch, readback
cmake/HcsShaders.cmake              compile + validate + embed a shader
cmake/EmbedSpirv.cmake              SPIR-V -> C++ byte array
tools/kernel-bench/kernel_bench.cpp CPU vs Vulkan, per shape, JSON (AGENTS.md 16.6 B3)
tests/unit/kernels/                 kernel-cpu: the reference and fp16/Q8_0 helpers
tests/integration/kernels_vulkan/   kernel-vulkan: real-device correctness (skips = NOT RUN)
```

## Q8_0 layout

Weights use the ggml `block_q8_0` layout so the kernel operates on real GGUF data:

```c
struct block_q8_0 { uint16_t d; int8_t qs[32]; };  // 34 bytes, value_i = fp16(d) * qs[i]
```

The shader reads the raw 34-byte blocks through a `uint` view; a small `load4` helper reconstructs four
consecutive bytes at any byte offset with at most two 32-bit loads, so no repacking is required.

## Build

The kernels need the **Vulkan SDK's `glslc` and `spirv-val` at build time only** (they are embedded in the
binary as SPIR-V, so the end user needs neither the SDK nor an import library — `vulkan-1.dll` is loaded
dynamically, `VK_NO_PROTOTYPES`):

```powershell
$env:VULKAN_SDK = "C:\VulkanSDK\1.4.357.0"
.\scripts\build.ps1 -Preset windows-msvc-release-kernels
```

The build compiles each `.comp` with `glslc --target-env=vulkan1.1`, validates it with `spirv-val
--target-env vulkan1.1`, and fails the build if either step fails. The engines stay CPU in this preset; use
`windows-msvc-release-vulkan` for the upstream Vulkan backends.

## API and the feature flag

```cpp
#include "hcs/kernels/cpu.hpp"
#include "hcs/kernels/vulkan.hpp"

hcs::kernels::vulkan::set_enabled(true);            // off by default
auto info = hcs::kernels::vulkan::device_info("Radeon");
auto r = hcs::kernels::vulkan::gemv_q8_0({x, w_q8_0, y, m, k}, "Radeon");
if (!r) {
    // Unsupported (flag off / no device) or BackendError -> keep the CPU result
    hcs::kernels::cpu::gemv_q8_0(x, w_q8_0, y, m, k);
}
```

* `gemv_q8_0` returns `Unsupported` while the flag is off, so a caller that tries the GPU first keeps a valid
  CPU path. The dispatch is the caller's decision; this module never silently changes an operator's backend.
* Two variants exist (`Variant::Scalar` = v1, the default; `Variant::Tiled` = a shared-memory experiment). The tiling
  hypothesis (x traffic dominates) was **rejected** on the 680M: v2 measured the same as v1 within noise
  (`hcs_kernel_bench`, `docs/research/vulkan-gemv-v2-shared-memory.md`). v1 stays the default; v2 stays behind the flag
  so the A/B is reproducible.
* Shape rules: `m > 0`, `k` a positive multiple of 32; null buffers are `InvalidArgument`.
* Invalid usage (`k % 32 != 0`, the flag off, no device) is rejected **before** any Vulkan object is created.

## Validation

* `kernel-cpu` (unit): fp16 conversion (round-trip, subnormals, `-0`, Inf), Q8_0 sizing, and the reference
  against an independent dequantise-and-dot loop.
* `kernel-vulkan` (real device): the kernel against the CPU reference over power-of-two and non-power-of-two
  shapes, plus determinism across repeated calls. With no Vulkan compute device the tests **skip (NOT RUN)**.
* `KernelVulkanFallback` (wave K): flag off -> the documented CPU fallback yields the reference result (no GPU
  needed); injected device loss -> `BackendError`, CPU fallback correct, next call recovers.
* `spirv-val` runs as part of every build of the shader.
* Validation layers (`VK_LAYER_KHRONOS_validation`, sync + best practices): 0 messages on the 6 `kernel-vulkan` tests and on
  the bench after the SPIR-V target fix (`docs/test-evidence/vulkan-kernels/m8-baseline/`). Run: set `VK_LAYER_PATH` to the
  SDK `Bin`, `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, `VK_LAYER_VALIDATE_SYNC=1`, then the test binary.

## Benchmark

```powershell
build\windows-msvc-release-kernels\bin\hcs_kernel_bench.exe --reps 20 --device Radeon
```

It reports, per shape, `cpu_ms_median`, `vulkan_ms_median`, `max_abs_err`, the CPU/Vulkan speed ratio and the
effective Q8_0 weight bandwidth. The measured numbers are recorded under `docs/test-evidence/` and summarised
in `docs/benchmarks.md`.

## Limits (read this before trusting the kernel)

* **Not default-enabled anywhere.** AGENTS.md M8: a kernel becomes default-on only with a repeatable measured
  benefit on supported hardware. This one is a candidate; the numbers, not the code, decide.
* **Measured against upstream: much slower (wave K, 2026-11-20).** GGML's Vulkan Q8_0 `mul_mat_vec`
  (m=4096, n=1, k=14336) takes 0.822 ms (70.7 GiB/s, weights resident); this kernel takes 14.99 ms per call (3.88 GiB/s,
  re-uploading the weights each call): 18.2x slower, 13.7x after subtracting the host weight copy. End to end the upstream
  Vulkan path decodes LFM3-Omni-2B Q8_0 at 39.7 t/s (CPU 19.3) and qwen3-0.6b Q4_K_M at 148.1 t/s (CPU 75.1). The kernel
  therefore stays default OFF. Numbers and method: `docs/test-evidence/vulkan-kernels/m8-baseline/`,
  `docs/research/vulkan-gemv-upstream-baseline.md`.
* **Correctness-first, not tuned.** v1 uses host-visible buffers (no staging), one thread per output row and no
  shared-memory reduction; a tuned version (thread blocks, subgroup reductions, staging) is a separate change.
* **Vulkan API floor:** the shader targets `vulkan1.1` (SPIR-V 1.3) to match the instance the kernel creates
  (`VK_API_VERSION_1_1`). It was `vulkan1.2` until the validation layers flagged `VUID-VkShaderModuleCreateInfo-pCode-08737`
  (SPIR-V 1.5 on a 1.1 instance); the device is queried at runtime, never assumed.
* **Device lost:** on `VK_ERROR_DEVICE_LOST` (submit or fence wait) the call returns `BackendError`, the cached context is
  destroyed and the next call rebuilds it. Covered by an injected-fault test (`inject_fault_for_testing`, test seam only);
  a real driver device loss was not provoked.
