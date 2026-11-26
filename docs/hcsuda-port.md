# HCSUDA port: unit -> source table

Source: `E:\hcslinux222\hcsuda` (Rust workspace, owner's own project), ported at hcsuda commit `94a3485` (the last
commit touching `hcsuda/`; repo HEAD `2b2eaa5`). Decision and scope: [ADR-014](decisions/ADR-014-hcsuda-port.md).
Every ported file starts with a `ported from hcsuda @94a3485` (or `Ported from`) note. "Rebuilt" means the idea and the
tested behaviour are kept but the code was written fresh for HCSInfer's architecture.

## Ported units

| HCSInfer unit | Source in hcsuda | How |
|---|---|---|
| `include/hcs/hardware/caps.hpp`, `src/hardware/caps.cpp` (`DeviceCaps`, `Support`, `Cap`, dtype table, `select_device`) | `crates/hcsuda-core/src/caps.rs` (`DeviceCaps`, `Features`, `Heap`, `Support`) | Rebuilt: tri-state `Cap` (Unknown/No/Yes) instead of `bool`, so "not reported" is distinguishable from "no". dtype table reduced to f32/f16/i8 (the dtypes that have kernels). |
| `src/hardware/caps_json.cpp` (probe JSON -> `DeviceCaps`) | `crates/hcsuda-vk/src/device.rs` (`query_features`, `dtype_path`) | Rebuilt: HCSInfer's probe already is `vulkan_info.cpp`; the decoder reads its JSON. Added to the probe: `integer_dot_product` accelerated flags. |
| `include/hcs/hardware/planner.hpp`, `src/hardware/planner.cpp` (`plan`, `gate_kernel`, kernel catalog) | `crates/hcsuda-core/src/planner.rs` (`plan`, `PathKind`, `Op`, `Tile`) + 7 planner unit tests | Ported + extended: evidence comes from a keyed profile instead of a `verified_paths` string set; capability gates per kernel (shared memory, invocations, push constants, subgroup 64 for coopmat, int-dot feature, alignment). Test cases ported: fresh device -> reference, unverified int path unused, verified int path used, coopmat needs f16 + alignment + verification, unsupported dtype stays on CPU, rope uses the scalar path only when verified, tile fits shared memory. |
| `include/hcs/hardware/kernel_profile.hpp`, `src/hardware/kernel_profile.cpp` (`evaluate_promotion`, `KernelProfile`) | `README.md` "The rules" (promotion needs correctness, determinism = 3 identical runs, >= 1.05x), `caps.rs::apply_selftest` (evidence must match device and driver), `report.rs::BenchRow` (median/spread, noisy > 0.25) | Rebuilt as code: the promotion rule was prose + CLI convention in hcsuda. New: keyed cache (device, driver, shader hash, bucket), atomic persistence, stale-entry handling, "default-enabled needs a measured upstream baseline". |
| `include/hcs/kernels/oracle.hpp`, `src/kernels/cpu/oracle.cpp` (`gemm_f32/f16/i8`, `softmax_rows`, `layernorm_rows`, `rmsnorm_rows`, `rope_rows`, `Tolerance`, `compare`) | `crates/hcsuda-core/src/reference.rs`, `crates/hcsuda-core/src/dtype.rs` (`Tolerance`, `for_len`, `accepts`, `max_error`) | Ported; accumulation is double precision (hcsuda summed in f32) so the oracle is the true value up to the final rounding. Tolerances unchanged: f32 rel 1e-5 + abs 1e-6 * sqrt(len). |
| `include/hcs/kernels/cpu_baseline.hpp`, `src/kernels/cpu/cpu_baseline.cpp` | none (hcsuda `docs/MEASURED.md` notes that its single-threaded oracle says nothing against a tuned multi-threaded CPU path) | New in HCSInfer. |
| `shaders/llm/gemm_f32_tiled.comp` | `crates/hcsuda-vk/shaders/gemm_tiled.comp` | Ported verbatim + provenance header. |
| `shaders/llm/gemm_f16_tiled.comp` | `.../gemm_f16_tiled.comp` | Ported verbatim. |
| `shaders/llm/gemm_i8dot_tiled.comp` | `.../gemm_i8dot_tiled.comp` (the untiled `gemm_i8dot.comp` is not ported: measured 4.2x slower in hcsuda) | Ported verbatim. |
| `shaders/llm/gemm_coopmat_f16.comp` | `.../gemm_coopmat.comp` | Ported verbatim; gated on subgroup size 64 and a reported 16x16x16 f16xf16->f32 subgroup-scope shape. |
| `shaders/llm/softmax_rows.comp`, `layernorm_rows.comp`, `rmsnorm_rows.comp` | `.../softmax.comp`, `layernorm.comp`, `rmsnorm.comp` | Ported verbatim. |
| `shaders/llm/rope_rows.comp` | `.../rope.comp` | Ported verbatim (the per-head variant `rope_heads.comp` is not ported). |
| `include/hcs/kernels/vk_runner.hpp`, `src/kernels/vulkan/vk_runner.cpp` (`Runner`, `Buffer`, `Pipeline`) | `crates/hcsuda-vk/src/{device,buffer,pipeline,dispatch}.rs` | Rebuilt in C++20: dynamic loader (no Vulkan import library), RAII with shared device ownership, device-local buffers with staged transfers, bounded fence waits, timestamp queries, validation layer + message counting, fault injection, `reset()` recovery. |
| `include/hcs/kernels/vk_ops.hpp`, `src/kernels/vulkan/vk_ops.cpp` (`KernelSet`, `PreparedOp`) | `crates/hcsuda-cli/src/gpu.rs` (host-side packing + dispatch geometry per kernel) | Rebuilt: host packing (f16 pairs, int8 transpose), grid sizes, push constant layouts, 65535-group guard. |
| `src/kernels/vulkan/tune.cpp` (`HCSInfer tune`) | `crates/hcsuda-cli/src/main.rs` (`cmd_selftest`: 3 runs + determinism; `cmd_bench`: warm-up, repeated samples, median/spread) | Rebuilt: one command that checks correctness, determinism, timing and the CPU baseline per kernel and shape, writes the keyed profile and an evidence JSON. |
| `tests/unit/planner/*`, `tests/unit/kernels_oracle/*`, `tests/kernels/vulkan_ops/*` | unit tests of `planner.rs`, `caps.rs`, `reference.rs` and the shapes of `selftest` (67x131x95 tail GEMM, 37x65x29 odd f16, 37x132x29 int8, 48x80x64 coopmat, 8x1024 row ops) | Cases ported, shapes extended (odd / tail / non-power-of-two). |

## Not ported (and why)

| hcsuda unit | Reason |
|---|---|
| `fp8.rs`, `quant.rs`, bf16/i4/fp8 dtypes | No HCS kernel uses them yet (the 680M reports no `VK_EXT_shader_float8`; FP8 is storage + conversion only). |
| `transformer.rs`, `eltwise.comp`, `attn_causal.comp`, `rope_heads.comp`, the `block` selftest | A full transformer block is the engines' job (llama.cpp); no HCS-owned attention is justified without a measured upstream bottleneck (AGENTS.md M8/M9). |
| `crates/hcsuda-io` (GGUF reader, dequant) | HCSInfer already has `gguf_probe` and the engines' loaders. |
| `saxpy.comp`, `triad.comp`, `gemv.comp` | Bandwidth micro-benchmarks / correctness-first GEMV; HCSInfer has the Q8_0 GEMV kernel for the decode shape. |
| CUDA driver shim (`nvcuda`) | Planned in hcsuda only. |

## Provenance and licence

hcsuda's README states GPL-3.0-or-later "while HCSUDA lives inside the HSCL1 repository" and calls a separate licence an
owner decision. The owner decided on 2026-11-25 (docs/roadmap/PLAN_V1_STABLE_NEXT.md, Wave C) that both projects are
the owner's own work and that the ported files are carried as the owner's code under HCSInfer's licence; the
provenance notes above and in each file header document where the code came from. See also `THIRD_PARTY_NOTICES.md`.
