# SUPERSEDED: `benchmark-quick-*` (2026-11-17)

The `benchmark-quick-*` files in this folder are the first, early quick-suite runs (build `fddc30df1218`, qwen3-0.6b Q4_K_M,
3 iterations, 199-token prompt). They are kept unedited as history and are **superseded** by the controlled wave-D runs
`docs/test-evidence/benchmarks/text-full-*.json` (static release-candidate EXEs, 5 iterations, median/p90, prefill and decode
sweeps, two models). Quote the `text-full-*` numbers, not these. In particular the "TTFT about 2.8x" line in `README.md` here
was for the 199-token prompt of that early run; it happens to agree with the wave-D qwen3-0.6b figure (444 ms CPU vs
156 ms Vulkan, 2.8x), but the two sets must not be mixed in one table.
