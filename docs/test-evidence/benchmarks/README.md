# Wave D benchmark evidence (2026-11-20)

Raw data behind `docs/benchmarks.md`. Produced by `tools/bench/run_matrix.py` (one section at a time, nothing concurrent)
with the final static EXEs of wave B; summarised by `tools/bench/make_report.py` into `summary.json`, the tables and charts.

| File pattern | Content |
|---|---|
| `environment.json` | EXE hashes, `doctor --json`, power plan, AC/battery, drive bus type, free RAM at start, repo HEAD |
| `text-full-<cpu\|vulkan>-<model>.json` (+ `.run.json`) | `HCSInfer benchmark --suite full`, 5 iterations, AGENTS.md 21.1 schema; `.run.json` = process exit code, wall, sampled peak memory |
| `llamabench-*.json` | upstream `llama-bench` pp512/tg128, 5 repetitions, raw `samples_ts` |
| `kv-<backend>-<type>-<ctx>.json` | `llama-bench` KV cache type x context (5 reps; CPU q8_0/q4_0 at 8192: 2 reps) |
| `vision-*.json`, `image-*.json`, `api-latency-*.json`, `hdd-read.json` | per-run records with exit codes, wall time, peak memory |
| `images/` | one rendered PNG per image configuration (not blank, 512x512) |
| `summary.json` | flat metric baseline for `tools/bench/check_regression.py` |

Result: every executed run exited 0 (PASS). NOT RUN: thermals, flushed-cache cold start, LPIPS/quality of renders and of
quantised KV caches, FLUX with N>1. The Vulkan "cold load" figures are not truly cold (the CPU run cached the file first);
see `docs/benchmarks.md`. A first attempt of the `kv` section on CPU was stopped by the operator after 4 files (quantised
KV at 8192 would have taken ~35 min per row at 5 repetitions) and resumed with 2 repetitions for those two rows.
No personal paths or tokens are stored; the API token used by the benchmark is a throwaway placeholder.
