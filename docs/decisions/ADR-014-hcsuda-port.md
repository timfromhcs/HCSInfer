# ADR-014: HCSUDA port - owner-authored Vulkan compute layer inside HCSInfer

Date: 2026-11-26 (decision of the owner 2026-11-25, implementation Wave B/C1+C2)
Status: Accepted, implemented (caps, planner, oracle, kernels, runner, `tune`); engine integration NOT done

## Context

HCSUDA (`E:\hcslinux222\hcsuda`, Rust) is a standalone Vulkan-first compute runtime by the same owner: a capability
probe, a conservative planner ("unknown = CPU"), a CPU oracle, GLSL kernels (tiled f32/f16 GEMM, int8-dot GEMM,
cooperative-matrix GEMM, softmax, LayerNorm, RMSNorm, RoPE) and a differential self-test, verified on the Radeon 680M
(`docs/MEASURED.md` in that repo). HCSInfer already has a Vulkan probe (`doctor`), a Q8_0 GEMV kernel and the M8 rules
(AGENTS.md 12, 13): one kernel = one feature with oracle, baseline, fallback and proof.

## Decision

1. Port, do not link. The Rust code is re-written in C++20 inside HCSInfer (no Rust toolchain, no FFI, no second
   runtime in the EXE). Every ported unit carries `ported from hcsuda @<commit>` in its header and is listed in
   `docs/hcsuda-port.md`. Both projects belong to the owner, so the licence is not a blocker; the licence text of
   HCSInfer is unchanged and the ported files are the owner's own code under that licence.
2. Where it lives:
   * `include/hcs/hardware/{caps,planner,kernel_profile}.hpp` + `src/hardware/` - typed capability record decoded from
     the doctor probe, the conservative planner, the promotion rule and the keyed profile cache (pure logic, no
     Vulkan, always built, `doctor --json` shows the result in a `planner` section);
   * `include/hcs/kernels/{oracle,cpu_baseline}.hpp` + `src/kernels/cpu/` - the CPU oracle (correctness) and a
     multi-threaded CPU baseline (speed);
   * `include/hcs/kernels/{vk_runner,vk_ops,vk_tune}.hpp` + `src/kernels/vulkan/` + `shaders/llm/` - standalone
     Vulkan compute runner, the kernels and `HCSInfer tune` (only with `HCS_ENABLE_VULKAN_KERNELS=ON`).
3. The standalone runner owns its own `VkInstance`/`VkDevice`/queue. It never shares objects with the GGML Vulkan
   backend (ADR-003). Every wait is bounded; a timeout or device-lost marks the runner lost; `reset()` is the
   recovery path; stale handles are refused.
4. Conservative by construction (AGENTS.md 12.1/13.3):
   * a capability the probe did not report is `Unknown` and counts as "not available"; the planner then returns the
     CPU reference;
   * a kernel is **promoted** only after correctness vs the CPU oracle AND 3 bit-identical runs AND >= 1.05x vs the
     baseline AND non-noisy timing (`evaluate_promotion`);
   * a promoted kernel is **default-enabled** only with a *measured* upstream GGML Vulkan op baseline >= 1.05x. `tune`
     cannot drive that baseline in-process cheaply, records it as NOT RUN, and therefore never enables anything by
     default. The `PlanPolicy::allow_unbaselined` seam exists for tests and experiments only;
   * evidence is keyed by (device id, driver, shader hash, shape bucket) so a driver update or a shader edit
     invalidates it; only entries that passed correctness are persisted.
5. Baseline choice: the single-threaded double-precision oracle is a correctness reference and a weak speed baseline,
   so promotion is decided against `cpu_mt_autovec` (row-parallel `std::thread`, float accumulation, compiler
   auto-vectorisation only). It is still a modest CPU baseline (no hand-written SIMD, no BLAS); results say "faster
   than that", never "faster than the CPU's best".

## Not done / explicitly out of scope here

* No kernel is wired into the llama.cpp / stable-diffusion.cpp engine path (that is C3: experiment with a
  `ggml_backend` op override, only if >= 1.05x vs upstream Vulkan, otherwise OFF).
* No GPU path for LoRA merge, quantisation or image metrics yet (C3a).
* The autotuner searches no candidate configurations (workgroup sizes, tiles); it measures the shipped kernels. TDR
  protection is a guard (shape/cold-dispatch limits, bounded waits, device-lost recovery), not dispatch splitting
  (that is C4).
* int4 / FP8 / bf16 kernels, attention, the transformer-block composition and the GGUF reader of HCSUDA are not ported.

## Risks

* A kernel that is correct on the 680M can be wrong or slow on another driver: evidence is per device+driver and the
  gates require the probe to report each feature; nothing is assumed from a device name. The coopmat kernel is
  additionally gated on `subgroupSize == 64` because its 64-lane workgroup must be exactly one subgroup.
* Sin/cos precision is only specified by Vulkan inside [-pi, pi]; RoPE is tested with a declared absolute tolerance of
  1e-3 for unit-scale inputs and the measured error is recorded next to it.
* Shader modules for int8-dot and cooperative matrix are compiled for Vulkan 1.2 / SPIR-V 1.5.

## Evidence

`docs/test-evidence/wave-b/c1/` (tune output, device/driver metadata, validation-layer result, test logs).
