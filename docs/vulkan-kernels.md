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

The build compiles each `.comp` with `glslc --target-env=vulkan1.2`, validates it with `spirv-val
--target-env vulkan1.2`, and fails the build if either step fails. The engines stay CPU in this preset; use
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
* Shape rules: `m > 0`, `k` a positive multiple of 32; null buffers are `InvalidArgument`.
* Invalid usage (`k % 32 != 0`, the flag off, no device) is rejected **before** any Vulkan object is created.

## Validation

* `kernel-cpu` (unit): fp16 conversion (round-trip, subnormals, `-0`, Inf), Q8_0 sizing, and the reference
  against an independent dequantise-and-dot loop.
* `kernel-vulkan` (real device): the kernel against the CPU reference over power-of-two and non-power-of-two
  shapes, plus determinism across repeated calls. With no Vulkan compute device the tests **skip (NOT RUN)**.
* `spirv-val` runs as part of every build of the shader.

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
* **No upstream-Vulkan baseline yet.** The benchmark compares against the *CPU reference*, not against GGML's
  Vulkan GEMV. That comparison needs the kernel wired into the GGML backend and is future work.
* **Correctness-first, not tuned.** v1 uses host-visible buffers (no staging), one thread per output row and no
  shared-memory reduction; a tuned version (thread blocks, subgroup reductions, staging) is a separate change.
* **Vulkan API floor:** the shader targets `vulkan1.2`; running it needs a driver that supports it. The target
  machine (Radeon 680M, AMD driver 32.0.21043.12001) is queried at runtime, never assumed.
* **No device-lost recovery path yet** beyond returning a `BackendError`; the context is rebuilt on the next call.
