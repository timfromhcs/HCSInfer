# Research note: Q8_0 GEMV v2 (shared-memory tiling of the activation vector)

Status: **rejected as a default** (measured, no benefit on the target device). Kept behind `Variant::Tiled` so the A/B
stays reproducible. Author: HCSInfer. Date: 2026-11-19.

Paper / DOI: none — this is an engineering experiment on our own kernel, not a paper port. Related: AGENTS.md 12
(kernel engineering) and 15.7 (research note template).

## Problem and hypothesis

HCS kernel v1 (`shaders/llm/gemv_q8_0.comp`) computes `y[m] = sum_k dequant(W[m,k]) * x[k]` with **one invocation per
output row**. Every invocation reads the whole activation vector `x` (k floats) from global memory, so the x traffic is
`m * k` floats — for a square matrix, as much as the weights (which are read once per row). Hypothesis: the kernel is
limited by redundant `x` traffic, and staging `x` in workgroup shared memory (one load per workgroup per K-tile) would
lift throughput substantially.

## Algorithm

v2 (`shaders/llm/gemv_q8_0_tiled.comp`, `Variant::Tiled`) keeps the same Q8_0/ggml weight layout and the same one-row-
per-invocation structure, but loops over K in tiles of 2048 floats: the whole workgroup cooperatively copies the tile
into `shared float xs[2048]`, `barrier()`, then each invocation accumulates its row's partial dot from shared memory.
Both barriers are outside the `row < m` branch so all invocations reach them.

## Requirements

* Same as v1: Vulkan 1.2 compute, no optional features; `shared` memory 8 KiB per workgroup; `local_size_x = 64`.
* No subgroup/fp16/int8 assumptions.

## Upstream comparison

v1 is the baseline for this A/B. GGML's own Vulkan GEMV (integer `q8_0 x q8_0` dot with dp4a-style paths) remains the
real reference and is **not** wired in yet (NOT RUN).

## Expected benefit and possible regressions

Expected: x traffic drops from `m * k` to about `(m / 64) * k` floats, i.e. up to ~64x less activation traffic. Risk:
the two barriers per tile serialise the workgroup and shared-memory bank conflicts could offset the gain, and the
extra instructions could dominate if the kernel is ALU-bound instead of bandwidth-bound.

## Implementation and flag

`hcs::kernels::vulkan::Variant::{Scalar,Tiled}`; default `Scalar`. `hcs_kernel_bench` measures both against the CPU
reference for every shape. No other code path uses the kernel.

## Test vectors and tolerance

`tests/integration/kernels_vulkan` runs both variants over shapes 1x32, 7x160, 64x256, 129x512, 512x1024, 1024x2048
against the CPU reference; tolerance `1e-3 * (1 + |y_cpu|)`. Both variants pass (max abs err ~1e-5 at k <= 1024).

## Benchmark method and baseline

`hcs_kernel_bench --reps 30 --device Radeon` on the Radeon 680M, CPU reference single-threaded, median of 30 timed
calls after warm-up. Raw: `docs/test-evidence/vulkan-kernels/gemv-q8_0-variants-bench.json`.

## Measured results

| m x k | CPU ms | v1 ms | v1 GiB/s | v2 ms | v2 GiB/s |
|---|---|---|---|---|---|
| 512x512 | 0.179 | 1.285 | 0.20 | 1.292 | 0.20 |
| 1024x1024 | 0.704 | 1.474 | 0.70 | 1.390 | 0.75 |
| 2048x2048 | 2.809 | 1.942 | 2.14 | 1.980 | 2.10 |
| 4096x4096 | 11.391 | 4.800 | 3.46 | 4.630 | 3.59 |
| 4096x11008 | 30.416 | 12.589 | 3.54 | 13.203 | 3.38 |
| 11008x4096 | 30.891 | 14.356 | 3.11 | 14.342 | 3.11 |

v2 is the same as v1 within run-to-run noise (better on one shape, worse on another). The hypothesis is **not
supported** on this device: the kernel is not limited by redundant `x` traffic at these sizes — the ~3.5 GiB/s ceiling
is set by the per-block byte extraction of the 34-byte Q8_0 layout and/or per-call overhead, not by x.

## Decision: reject (keep v1 as the default)

* `Variant::Scalar` (v1) stays the default: it is simpler and equally fast.
* `Variant::Tiled` (v2) is kept behind the flag so the A/B is reproducible; it is not used by anything else.
* No default-enable: the kernel still loses to GGML's Vulkan GEMV baseline (NOT RUN) and is not on the load path.

## Limitations and next hypotheses

* The byte-extraction cost of the 34-byte block stride is the prime suspect; a variant that copies weights into a
  4-byte-aligned staging buffer once and then reads aligned words, or an integer `q8_0 x q8_0` dot (quantise `x` to
  int8 per 32-block and use integer dot products like GGML), would test that. Neither is implemented.
* Per-call overhead (~1.3 ms: buffer create/map/dispatch/readback) dominates small shapes; persistent buffers would
  remove it. Out of scope for the correctness-first kernel.
