# Wave K - M8 baseline: upstream GGML Vulkan vs the HCS Q8_0 GEMV (2026-11-20)

Machine: Windows 11 build 10.0.26200, AMD Ryzen 7 7735HS, Radeon 680M (`AMD Radeon(TM) Graphics`, vendor 0x1002,
device 0x1681, UMA, fp16 yes, int-dot yes, warp size 32, shared memory 32768, matrix cores none), AMD proprietary
driver 2.0.353 / `26.6.1`, Vulkan API 1.4.315 (as reported by `vulkaninfo --summary`), 21 GB RAM.
Vulkan SDK 1.4.357.0 (validation layers, `glslc`, `spirv-val`). llama.cpp at the pinned `d3c3e24`
(`llama-bench --version` -> `version: 1.0.0-dev (build 1, commit d3c3e24)`, MSVC 19.44.35228), using the existing builds
`build/llama-vulkan` and `build/llama-cpu` of the main checkout (unchanged pin, no local patches). The HCS kernel
binaries were rebuilt in this worktree (`windows-msvc-release-kernels`) from the sources in this commit.

Models (both real GGUF):

| Model | Quant | Bytes | SHA-256 |
|---|---|---|---|
| `qwen3-0.6b-q4_k_m.gguf` | Q4_K_M | 484,219,712 | `4ea794120a3b2244a6e8a36354f1aa53e7f0c52b2f1f280dde0d7f522f37ae7c` |
| `LFM3-Omni-2B-Q8_0.gguf` (lfm3 1.7B) | Q8_0 | 1,837,991,168 | `59118f661eaacb93d204723d76355b53347f0940b508c11412f6290fd49158d4` |

## 1. Upstream end-to-end: `llama-bench` (pp512 / tg128, `-ngl 99` for Vulkan, `-t 8` for CPU)

Files: `vulkan-*-cold.json` (`-r 1 --no-warmup`), `vulkan-*-warm.json` and `cpu-*-warm.json` (`-r 5`, default
warm-up), raw `samples_ts` inside. "cold" = first process run without warm-up (includes shader/pipeline creation); the
**OS file cache is not flushed** (needs administrator rights), so it is process-cold, not disk-cold. Median / p90 of 5.

| Model | Backend | pp512 t/s median / p90 | tg128 t/s median / p90 |
|---|---|---|---|
| qwen3-0.6b Q4_K_M | Vulkan warm | 2062.5 / 2080.5 | **148.1 / 150.1** |
| qwen3-0.6b Q4_K_M | Vulkan cold (n=1) | 527.4 | 146.0 |
| qwen3-0.6b Q4_K_M | CPU warm | 508.1 / 522.3 | 75.1 / 75.4 |
| LFM3-Omni-2B Q8_0 | Vulkan warm | 806.4 / 842.6 | **39.7 / 39.8** |
| LFM3-Omni-2B Q8_0 | Vulkan cold (n=1) | 283.9 | 38.6 |
| LFM3-Omni-2B Q8_0 | CPU warm | 86.9 / 87.9 | 19.3 / 19.5 |

Effective decode weight bandwidth of the upstream Vulkan path (tg t/s x model bytes): Q8_0 2B = 73.0 GB/s
(68.0 GiB/s), Q4_K_M 0.6B = 71.7 GB/s - the practical memory-bandwidth ceiling of this UMA APU for decode.

## 2. Upstream per-operator: `test-backend-ops perf -o MUL_MAT -b Vulkan0`

`test-backend-ops-mul-mat-perf.txt` (stopped manually after the cases below were captured; the full sweep is long;
ANSI colours stripped). The matching decode shape (n=1) with resident weights:

| Case | us/run | weight bytes | effective |
|---|---|---|---|
| `MUL_MAT q8_0 m=4096 n=1 k=14336` | **822.06** | 62,390,272 | 75.9 GB/s (70.7 GiB/s) |
| `MUL_MAT q4_K m=4096 n=1 k=14336` | 432.52 | - | - |

## 3. HCS Q8_0 GEMV, isolated (`hcs_kernel_bench --reps 30 --device Radeon`, 3 independent runs)

Files `kernel-bench-run{1,2,3}.json`; run-to-run spread <= 3 %. Median ms per call of the three runs; every call
allocates host-visible buffers, copies the whole weight matrix, dispatches and reads back:

| m x k | CPU ref (1 thread) | host weight-copy floor | HCS v1 scalar | HCS v1 GiB/s | upstream Vulkan (same shape, resident) |
|---|---|---|---|---|---|
| 512x512 | 0.19 | 0.01 | 1.41 | 0.18 | - |
| 2048x2048 | 3.14 | 0.12 | 2.03 | 2.05 | - |
| 4096x4096 | 12.58 | 1.04 | 4.85 | 3.42 | - |
| 4096x11008 | 33.86 | 2.78 | 12.02 | 3.71 | - |
| 4096x14336 | 44.09 | 3.66 | **14.99** | 3.88 | **0.822** |

At 4096x14336 the HCS call is **18.2x slower** than the upstream operator; after subtracting the whole host weight
copy (3.66 ms) it is still 13.7x slower. The comparison is not like-for-like: upstream keeps weights resident, the HCS
kernel re-uploads them on every call. That is itself a finding - the kernel has no resident-weight mode, so it cannot
sit on the decode path as is. The `tiled` variant again equals `scalar` within noise.

## 4. Validation layers (`VK_LAYER_KHRONOS_validation`, SDK 1.4.357.0)

`kernel-vulkan-tests-validation-layers-BEFORE-fix-spirv15.txt`: **real finding.** With the layer loaded
(`VK_INSTANCE_LAYERS`, `VK_LAYER_PATH`, synchronization + best-practices validation) the kernel produced
`VUID-VkShaderModuleCreateInfo-pCode-08737`: the shader was compiled with `--target-env=vulkan1.2` (SPIR-V 1.5) but the
kernel's instance requests Vulkan 1.1 (SPIR-V 1.3), so the module was invalid for its instance. The AMD driver
tolerated it; the spec does not. Hypothesis 1 (target-env mismatch) was fixed by compiling and validating with
`vulkan1.1` (the shader uses only `#version 450` core features). After the fix
`kernel-vulkan-tests-validation-layers.txt` has **0 validation errors, 0 warnings, 0 sync and 0 best-practices
messages** (`PASSED 6 tests`), and `kernel-bench-validation-layers.out` (bench under the layer) has 0 validation
messages. `kernel-vulkan-tests-no-layers.txt` is the same suite without layers (6/6 PASS; captured before the
SPIR-V target change - the suite was re-run after it with the layers on).

## 5. Fallback tests added

`KernelVulkanFallback.*` in `tests/integration/kernels_vulkan`: (a) flag off -> the documented caller fallback gives
the reference result (needs no GPU); (b) an injected `VK_ERROR_DEVICE_LOST` at submit (test seam
`inject_fault_for_testing`, real queue untouched) -> `BackendError` "device lost", cached context torn down, CPU fallback
correct, next call recovers and matches the reference. A real driver device loss was NOT provoked.

## 6. HCS control-plane benchmark (AGENTS.md 21.1 JSON)

`hcs-benchmark-vulkan-*.json` (`benchmark --suite quick --backend vulkan --iterations 5 --max-tokens 64`; the binary
reports `git_commit` 037e5831ce57, the build it was made from; Vulkan preset). Cold = first process run, no warm-up.

| Model | cold decode t/s | warm decode t/s median / p90 | cold TTFT ms | warm TTFT ms median / p90 | warm load ms median / p90 |
|---|---|---|---|---|---|
| qwen3-0.6b Q4_K_M | 80.1 | 122.3 / 123.4 | 1743 | 141 / 146 | 408 / 412 |
| LFM3-Omni-2B Q8_0 | 34.3 | 35.3 / 36.2 | 1129 | 351 / 434 | 928 / 946 |

(HCS decode is below raw `llama-bench` because it includes sampling, detokenisation and the chat-template path.)

## Decision

The HCS Q8_0 GEMV is **13.7-18x slower per call than the upstream Vulkan `mul_mat_vec` on the matching shape**, and its
3.9 GiB/s is ~5 % of the ~70 GiB/s the upstream path already reaches on this APU. No repeatable benefit exists, so the
kernel stays **default OFF** and off the model load path. A future attempt needs resident weights, a subgroup-reduction
GEMV with several rows per workgroup, and a shape-matched comparison via `test-backend-ops` before anything else.
