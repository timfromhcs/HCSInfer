# Research note: Q8_0 GEMV vs the upstream GGML Vulkan baseline (wave K)

Status: **rejected as a default; deferred** (measured). Author: HCSInfer. Date: 2026-11-20. Evidence:
`docs/test-evidence/vulkan-kernels/m8-baseline/` (its README has every number). Related: AGENTS.md 12, 15.7, 16.6 B1/B3, 21.

Paper / DOI: none (engineering measurement). Upstream examined: llama.cpp `d3c3e24`, `ggml/src/ggml-vulkan` `mul_mat_vec`.

## Problem and hypothesis
M8 says profile upstream first. H1: the HCS Q8_0 GEMV (v1/v2) can beat GGML's Vulkan `mul_mat_vec` on the Radeon 680M
for batch-1 decode. H2: the earlier 2.1-2.5x win over the CPU *reference* means the kernel is useful.

## Method
Upstream: `llama-bench` pp512/tg128, 5 repetitions plus a cold single run, qwen3-0.6b Q4_K_M and LFM3-Omni-2B Q8_0, Vulkan
and CPU; `test-backend-ops perf -o MUL_MAT -b Vulkan0` for the per-operator number. HCS: `hcs_kernel_bench --reps 30`, 3
runs, on the added shape 4096x14336 that equals the upstream test case. Same device, driver and commit; GPU otherwise idle.

## Results
| Quantity | Value |
|---|---|
| Upstream Vulkan decode, Q8_0 2B | 39.7 t/s (p90 39.8) = 73 GB/s of weights; CPU 19.3 t/s |
| Upstream Vulkan decode, Q4_K_M 0.6B | 148.1 t/s (p90 150.1); CPU 75.1 t/s |
| Upstream op Q8_0 4096x14336 n=1 | 0.822 ms = 70.7 GiB/s |
| HCS v1, same shape (per call, incl. upload) | 14.99 ms = 3.88 GiB/s; 18.2x slower (13.7x after removing the host weight copy) |
| HCS v2 (tiled) | same as v1 within noise |

H1 **rejected**. H2 **rejected as a criterion**: the CPU reference is single-threaded and is not the baseline that matters.

## Cause (partly untested)
Each call allocates host-visible buffers and re-uploads the weights (the copy alone is ~3.7 ms of ~15 ms); one invocation
per row with no subgroup reduction. Neither can approach the ~70 GiB/s the upstream kernel streams. The split between
allocation, first-touch of mapped memory and the shader itself was not measured separately (no GPU timestamps yet).

## Validation finding
Validation layers exposed a real defect: a SPIR-V 1.5 shader (`vulkan1.2`) on a Vulkan 1.1 instance
(`VUID-VkShaderModuleCreateInfo-pCode-08737`). Fixed by compiling for `vulkan1.1`; 0 messages afterwards.

## Decision
Nothing is default-on. The kernel stays OFF; the code stays as the toolchain proof. Next attempt, only if a model profile
shows a GGML hotspot: resident weights, subgroup-reduce multi-row workgroups, GPU timestamp queries, compare against
`test-backend-ops` on the same shape, and only then touch the end-to-end path. Limits: one device, one driver, one
shape family.
