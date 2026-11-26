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

## HCSUDA-ported kernel set and `HCSInfer tune` (wave B / c1, 2026-11-26)

Eight kernels ported from the owner's own HCSUDA project (provenance: [`hcsuda-port.md`](hcsuda-port.md), decision:
[ADR-014](decisions/ADR-014-hcsuda-port.md)): tiled f32 / f16 GEMM, int8-dot GEMM, cooperative-matrix f16 GEMM, row
softmax, LayerNorm, RMSNorm and RoPE. They are driven by `src/kernels/vulkan/vk_runner.cpp` / `vk_ops.cpp`, planned by
`src/hardware/planner.cpp` (capability-gated, unknown = CPU) and benchmarked by `HCSInfer tune`. Nothing is wired into
the llama.cpp / stable-diffusion.cpp engines.

**Correctness (real AMD Radeon 680M, driver 26.6.1, Vulkan 1.4.315, subgroup 64).** `hcs_test_vulkan_ops` (ctest label
`kernel-vulkan`) compares every kernel with the CPU oracle on odd, tail and non-power-of-two shapes with abs+rel
tolerances, runs each prepared op three times and requires bit-identical output, checks invalid inputs, injected
device loss and dispatch timeout with recovery, and the Khronos validation layer reported 0 messages (16/16 PASS,
`docs/test-evidence/wave-b/c1/vulkan_ops_run.txt`). All 10 SPIR-V modules pass `spirv-val` (ctest `spirv-val.*`).
The coopmat and int8-dot kernels ran because this device reports `cooperative_matrix` (16x16x16 f16xf16->f32,
subgroup scope) and `integer_dot_product`; on a device that does not, they are reported `NOT RUN on this hardware`.

**RoPE trig tolerance (measured).** Vulkan only bounds `sin`/`cos` inside [-pi, pi], and RoPE angles grow with the
position. Declared tolerance: abs 1e-3 for unit-scale inputs. Measured max abs error against the double-precision
oracle: 0 (1x2), 2.6e-7 (7x64), 4.4e-6 (40x126, pos0 17), 8.8e-6 (129x64, pos0 3, base 5e5), 4.4e-5 (512x128) in the
unit test and 4.0e-5 (512x128) / 2.0e-4 (2048x128) in `tune`. The error grows roughly linearly with the position, so
the 1e-3 tolerance holds with about 5x margin at 2048 rows and would be exceeded at several thousand positions: RoPE
is validated only for the shapes in the evidence, and a longer-context bucket needs its own measurement.

**`tune --suite full` (21 samples per bucket).** Median wall time of one dispatch with data resident on the device,
against a 16-thread `std::thread` row-parallel, compiler-auto-vectorised CPU baseline (not tuned BLAS, not upstream
GGML):

| kernel | correct (3/3 identical runs) | worst abs err | GPU GFLOP/s (min-max over buckets) | wall speedup vs CPU baseline | promoted buckets |
|---|---|---|---|---|---|
| `gemm_coopmat_f16` | 4/4 | 1.9e-05 | 635.3-648.8 | 7.13-21.13x | 2/4 |
| `gemm_f16_tiled` | 4/4 | 7.2e-05 | 377.2-565.3 | 5.63-17.53x | 4/4 |
| `gemm_i8dot_tiled` | 4/4 | 0 | 524.3-731.7 | 6.80-25.83x | 4/4 |
| `gemm_f32_tiled` | 4/4 | 7.2e-05 | 360.3-562.7 | 6.07-14.72x | 4/4 |
| `softmax_rows` | 3/3 | 3.7e-09 | 9.3-49.5 | 2.23-8.07x | 3/3 |
| `layernorm_rows` | 3/3 | 4.8e-07 | 10.1-72.2 | 1.77-6.00x | 3/3 |
| `rmsnorm_rows` | 3/3 | 4.8e-07 | 6.8-40.7 | 1.29-4.46x | 3/3 |
| `rope_rows` | 2/2 | 0.0002 | 41.6-49.6 | 4.56-6.48x | 1/2 |

Five buckets were correct but **not promoted** because their timing spread exceeded 0.25 (`timing too noisy`); the
numbers are not used. The speedups compare against a naive CPU baseline on a shared-memory APU and say nothing about
the upstream GGML Vulkan backend.

**Promotion status (honest).** The planner rule is: correct AND 3 bit-identical runs AND >= 1.05x the baseline gives
*promoted*; default-enabling additionally needs a **measured upstream GGML Vulkan baseline**, which `tune` records as
`NOT RUN`. Result on this machine: all 8 kernels are *promoted* (some buckets) against the CPU baseline and **none is
default-enabled**; `doctor --json` -> `planner` shows every op as `reference` (CPU) with the candidate listed.
Evidence: `docs/test-evidence/wave-b/c1/` (`tune-evidence.json`, `doctor-with-profile.json`, `vulkan_ops_run.txt`,
`tune-validation-stdout.txt`). The profile is cached under `HCS_DATA_DIR` as `kernel-profile.json`, keyed by device,
driver, shader hash and shape bucket.

## Upstream GGML Vulkan operator baseline and the promotion decision (wave C / k1, 2026-11-26)

**What was missing.** Wave B / c1 recorded the upstream baseline as `NOT RUN`, so no kernel could be default-enabled.
Wave C / k1 drives the pinned ggml Vulkan backend (llama.cpp `d3c3e24`, the same GGML the engines run) **in-process**:
`src/kernels/vulkan/ggml_vk_baseline.cpp` (library `hcs_kernels_ggml_baseline`, built only with Vulkan + kernels + the
llama.cpp engine ON) builds a one-node ggml graph per operation (`ggml_mul_mat` on F32 / F16 / Q8_0 / Q4_K weights,
`ggml_soft_max`, `ggml_norm`+`mul`+`add`, `ggml_rms_norm`+`mul`, `ggml_rope_ext` NEOX), keeps the inputs device
resident, times `ggml_backend_graph_compute` (submit + wait, the same wall-clock method as the HCS number: one
submit + fence) after a warm-up, and spot-checks every result against a double-precision reference so a wrong-but-fast
operation is never recorded. `HCSInfer tune` calls it for every bucket and writes `upstream_ggml_vulkan`
`{status, median_ms, spread, variant, ratio_upstream_over_hcs}` into the profile; `tune --no-upstream` skips it.
For F16 GEMM the faster of {F32, F16} activations is the baseline (conservative); there is **no** upstream operator
with int8 x int8 -> int32 semantics, so `gemm_i8dot_tiled` is reported `not_comparable` and can never be
default-enabled (its only possible comparison, a Q8_0 `mul_mat`, has different semantics). A bucket whose upstream
timing spread exceeds 0.25 is "not a result" (new in `evaluate_promotion`). Standalone tool:
`hcs_ggml_vk_bench --suite quick|full|llm [--out file.json]` (AGENTS.md 21.1 metadata; label `performance`; real-device
tests `tests/performance/ggml_vk_baseline`, 7/7 PASS on the 680M).

**Decision rule across repeated runs.** `scripts/kernel-decision.ps1 -Evidence run1.json run2.json run3.json` marks a
(kernel, bucket) promoted only if, in **every** run, it is correct, 3/3 bit-identical, has spread <= 0.25 (HCS and
upstream) and beats both the CPU baseline and the measured upstream operator by >= 1.05x; fewer than `-MinRuns` (3)
runs promotes nothing. Reads files only.

**Measured (AMD Radeon 680M, driver 26.6.1, Vulkan 1.4.315, `tune --suite llm`, 15 samples, ONE complete run;
evidence `docs/test-evidence/wave-c/k1/`).** Ratio = upstream ms / HCS ms (> 1: HCS faster). Full per-bucket table:
`docs/test-evidence/wave-c/k1/single-run-table.md`.

| kernel | buckets compared | ratio range (upstream / HCS) | buckets >= 1.05x |
|---|---|---|---|
| `gemm_coopmat_f16` | 9 | 0.24x - 0.84x | 0 |
| `gemm_f16_tiled` | 8 | 0.20x - 1.87x (1.87x is the 128^3 bucket, spread 0.475: not a result) | 0 valid |
| `gemm_f32_tiled` | 8 | 0.24x - 0.99x | 0 |
| `gemm_i8dot_tiled` | 0 | `not_comparable` | - |
| `softmax_rows` | 5 | 0.77x - 1.04x | 0 |
| `rmsnorm_rows` | 5 | 0.72x - 1.06x | 1 (upstream spread 0.26: not a result) |
| `rope_rows` | 4 | 0.66x - 1.10x | 2 (upstream spread 0.17 / 0.45) |
| `layernorm_rows` | 5 | 0.89x - 2.22x | 4 |

* **GEMM: the HCS kernels lose clearly (2x to 5x slower) on every real LLM / SD shape**, e.g. 512x4096x4096 F32:
  HCS 43.9 ms (coopmat f16) / 25.4 ms (int8) vs upstream F16 10.6 ms; decode GEMV 1x4096x4096: HCS f32 1.80 ms vs
  upstream F32 1.00 ms, upstream Q8_0 0.32 ms, Q4_K 0.24 ms. Upstream additionally reads quantised weights directly
  (Q4_K / Q8_0 `mul_mat`, 104-226 GFLOP/s in decode, 1.7 TFLOP/s in prefill on this iGPU), which no HCS kernel does.
  This is a decisive negative result, not a noise question.
* **softmax / RMSNorm / RoPE: parity (0.7x-1.1x)**; every >= 1.05x bucket has a noisy upstream timing or is a single
  run. RoPE at the Llama shapes is within noise of upstream.
* **LayerNorm: the only family that beat upstream (1.47x-2.22x on 4 of 5 buckets in one run)**. Honest reasons: the
  HCS kernel is one fused dispatch while upstream's `norm`+`mul`+`add` chain is three. Two of those buckets also had
  upstream spread 0.27-0.30 (> 0.25) and the other two were measured once, so **nothing is promoted** (rule: 3 runs).
  Real models that use LayerNorm run it at m x n = 77x768 (CLIP), 729x1152 (SigLIP), 4096x320 (SD1.5 UNet block);
  those buckets were added to the `llm` suite after the run and are NOT RUN.
* **Repeat runs: BLOCKED.** The first complete run finished at 21:31; the second/third whole-suite runs (with the
  fixes below) could not start because `D:\HCSScratch\realmodel.lock` was held by an empty orphan file (0 bytes,
  created 21:34:52 by another agent's wrapper, no process holding it, no real-model process on the machine). It was
  not removed because it is not this task's lock. 45-minute waits timed out twice (`docs/test-evidence/wave-c/k1/
  lock-timeout.txt`). The 3-run decision is therefore **NOT RUN**.

**Oracle fix found by the run (not re-verified on the device: NOT RUN).** At k = 4096 five GEMM buckets reported
`FAIL (wrong result)` with unit-range inputs (max abs error 1.4e-4 to 4.3e-4 against a sqrt(k)-scaled tolerance of
6.4e-5, on cancelling elements). The float32 accumulation error of a length-k sum grows ~ k * u * rms(partial sum), so
this is an oracle-data scale problem (the coopmat kernel, whose hardware accumulation orders differently, passed at
7.6e-5), not a proven kernel defect. `tune` now shrinks the float operands by (1024/k)^(1/4) each for k > 1024 (the
variance-preserving scale real weights have; k <= 1024 evidence is unchanged) and does **not** touch any tolerance.
Whether the 5 buckets now pass is untested.

**Final state: default OFF for every kernel.** No kernel is promoted (single run, GEMM loses outright), so the
planner keeps routing every op to the reference (CPU) path and the engines never call an HCS kernel. Integration
points: (a) HCS-owned operations - the only GEMM-like one is `lora_rows` in `src/merge/math.cpp` (rank-r double
accumulation, single-threaded, followed by a streamed write with a SHA-256 manifest); it was **not** moved to the GPU:
its output must stay bit-reproducible for the manifest (double vs the kernel's float), and a 4096x4096 layer
(64 MiB f32) is bounded by the write, not the arithmetic - this is an estimate, not a measurement; (b) a ggml
backend op override was **not** implemented: no measured >= 1.05x win exists for a shape the models run (GEMM loses;
LayerNorm was not measured at model shapes and repeated), and a fused LayerNorm saving ~0.1-0.4 ms per op would be
roughly 1% of an SD step even if it held (an estimate, not a measurement). So no end-to-end token-identity / tokens-per-second comparison applies (nothing
changed in the engine path).

**Autotuner reset and the TDR guard.** `HCSInfer tune --reset [--device <sel>] [--kernel <id>]` drops only the
selected device's (and kernel's) saved evidence from `<state-dir>/kernel-profile.json` so the next `tune` re-benchmarks
from scratch; a corrupt profile is reported and never replaced (`TuneCli.*`, 4/4 PASS on the device; `tune --show`
prints the table without GPU work). Stale evidence (other driver, other shader hash) is never matched in any case. TDR
protection: one dispatch whose first (cold) run takes > 1.5 s is abandoned (`kColdAbortMs`), the big shapes are
ordered ascending so the duration of the next one is predicted from the measured rate, and every runner call checks
`VK_ERROR_DEVICE_LOST` and a dispatch timeout: `VulkanRunner.InjectedDeviceLostIsReportedRefusesWorkAndRecoversAfterReset`
and `...DispatchTimeoutMarksTheRunnerLostAndResetRecovers` pass on the real device
(`docs/test-evidence/wave-c/k1/vulkan_ops_tests.txt`, 20/20). These are soft simulations; a real OS TDR reset was
not provoked and is NOT RUN.
