# Wave A / a2: image-generation equivalence and reproducibility (plan items I3/I4)

Machine: Windows 11 26200, Ryzen 7 7735HS, 21 GB shared RAM, Radeon 680M (Vulkan0, AMD proprietary driver).
Binary: `build/sd-upstream-ggml/bin/sd-cli.exe` (stable-diffusion.cpp pin `532d2c15`, `SD_USE_UPSTREAM_GGML=ON`,
`SD_VULKAN=ON`). Model: `sd15-q8_0.gguf` (SD 1.5 Q8_0). Settings for every run: 512x512, 20 steps, seed 42,
cfg 7, Euler a, prompt "a red apple on a wooden table, studio lighting, photo". Scripts: `run_matrix.sh` /
`run_stress.sh` (kept in `D:\HCSScratch\a2`, command line is in `matrix/*.log`). Metrics: `tools/imgqa/compare.py`
(numpy only; PSNR 8-bit, SSIM on luma). LPIPS: NOT RUN (needs torch + weights).

All PNGs listed below were opened and looked at: every one of them is a clean, sharp red apple on a wooden table
with the same composition. I could not tell any two of the 512x512 images apart by eye (the amplified diff image
`diff_cpu_vs_vulkan0disk_x8.png` shows only low-amplitude speckle, no structure).

## Result 1: parameter placement does not change the picture (PASS)

| compute | params | repeats | distinct sha256 | note |
|---|---|---|---|---|
| cpu | cpu | 3 | 1 (`65260ede`) | |
| cpu | disk | 3 | 1 (`65260ede`) | byte-identical to cpu:cpu |
| vulkan0 | cpu | 5 | 2 (`9b09aa7c` x4, `18a80ff8` x1) | see result 3 |
| vulkan0 | disk | 3 | 1 (`9b09aa7c`) | byte-identical to the dominant vulkan0:cpu output |

Full pairwise PSNR/SSIM table: `equivalence.md` / `equivalence.json`. cpu vs vulkan0: PSNR 39-41 dB, SSIM 0.989-0.991,
about 2-4 % of pixels differ by more than 8/255, max abs diff 66-81. That is ordinary CPU-vs-GPU float rounding and
is visually identical.

## Result 2: the "very different, degenerate" B7 image was a 2-step artefact, not a params-backend bug

`docs/test-evidence/B7-params/` was 256x256 with `--steps 2`. I opened `compute-cpu_params-cpu.png` (CPU compute, CPU
params) and `compute-vulkan0_params-disk.png`: both are the same smeared colour blobs. A 2-step Euler-a sample of SD 1.5 is
unconverged, so it looks degenerate in every configuration, including the cpu+cpu reference. With 20 steps at 512x512
all four configurations give a proper image (result 1). The remaining B7 hash difference (a vs b) is explained by
result 3 (Vulkan is not bit-reproducible), not by `--params-backend`.
Classification: **configuration / evidence-design** (too few steps), not code, driver or model. The HCS adapter maps
`params_backend`, backend, model path and threads correctly (`src/engines/diffusion/diffusion_engine.cpp`); seed and
sampler are per-request parameters that were not involved.

## Result 3: Vulkan is not run-to-run bit-reproducible, but the differences are not visible (FAIL for byte reproducibility, PASS for visual)

Same configuration (`vulkan0`, `cpu` params), same seed. sha256 over all Vulkan runs of this session (matrix 8, plus
cold smoke 1, plus 2 under CPU load):

| run | context | sha256 (first 8) | vs dominant `9b09aa7c`: PSNR / SSIM / max diff |
|---|---|---|---|
| matrix r2-r5, disk r1-r3, load r2 | warm / normal | `9b09aa7c` | identical |
| matrix r1 | first run of the batch | `18a80ff8` | 41.5 dB / 0.9907 / 91 |
| smoke r1 | first run after a long idle | `bbf72044` | 46.0 dB / 0.9937 / 50 |
| stress r1 | first run, 16 busy CPU threads | `0eab2c45` | 42.9 dB / 0.9916 / 86 |

So: 4 of the 5 matrix repeats are bit-identical, the fifth differs; a total of 4 distinct outputs in 11 completed Vulkan runs,
8 of them identical. The differences are noise-level (SSIM > 0.99, <=1 % of pixels off by more than 8/255, no
structural change, confirmed by eye). The CPU backend was bit-reproducible (3/3 and 3/3).

Root cause (partially proven). The logs of every Vulkan run contain a *timing probe*:
`flash attention auto: Vulkan0 -> off (probe: normal 4.372 ms, flash 7.646 ms)` and
`conv2d direct auto: ... (probe output check failed)`. The kernel choice is made from measured milliseconds. In
`stress_cpuload/vulkan0_cpu_cpuload_r1.log` the probe read `normal 20.948 ms, flash 10.886 ms` and selected flash
attention **on**; that run has its own hash. That is direct evidence that the probe changes the output. Not explained:
matrix r1 and smoke r1 chose flash "off" like the warm runs yet still produced different hashes; the first run of a
process batch after idle seems to differ for another reason (cold GPU clocks / first-time driver pipeline state / other
probe), which I could not isolate. Classification: **dependency (upstream auto-probe) with a possible driver
component**. Hypothesis "pinning `--flash-attn off --conv-direct off` makes Vulkan output stable even for the first
run and under CPU load" was prepared (`run_stress_faoff.sh`, 3 idle + 3 loaded runs) but **NOT RUN to completion**:
the single shared real-model lock was held by other agents for more than 60 minutes. Status: **BLOCKED (measurement)**.

## HCS change (code fix, unit-tested; real-run validation NOT RUN)

The adapter used to leave `flash_attn_mode` / `conv_direct_mode` at upstream "auto". It now pins both to `off` on GPU
backends (`include/hcs/engines/diffusion_runtime.hpp`, wired in `DiffusionImageEngine::load`; CPU keeps upstream
behaviour because its "auto" does no probe). `off` is also what the probe chose on this GPU in 12 of 13 probes, so
no speed loss is expected (flash measured about 2x slower than normal attention on the 680M). Unit test
`DiffusionRuntime.*` (2 tests) passes; the engine file was compile-checked with cl, but was not linked/run against a
real model in this worktree (external submodules are not checked out here). Required follow-up: run the forced-off
experiment and the diffusion integration test once the lock is free; until then the fix is **unproven**.

## Tooling

`tools/imgqa/compare.py` (metrics), `report.py` (equivalence table writer), `test_imgqa.py` (7 self-tests, pass:
`python tools/imgqa/test_imgqa.py`).

## Verdict table

| item | status |
|---|---|
| I3 cpu/disk/vulkan params equivalence | PASS (identical on CPU; Vulkan cpu-vs-disk identical when warm; cpu-vs-vulkan SSIM ~0.99) |
| I3 B7 degenerate image root cause | PASS (2-step artefact, reproduced on cpu+cpu) |
| I4 Vulkan byte reproducibility | FAIL (4 distinct hashes in 11 runs; differences invisible) |
| I4 pinned-kernel fix validated on hardware | BLOCKED (lock contention) |
| LPIPS | NOT RUN |
