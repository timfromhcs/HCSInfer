# Benchmarks

Everything here is copied from the evidence files named in each section; nothing is estimated and nothing is claimed
beyond those files. Status words: PASS / NOT RUN / BLOCKED. Methodology decision: `decisions/ADR-010-benchmark-methodology.md`.
All numbers come from **one machine** (Windows 11 build 10.0.26200.9974, Ryzen 7 7735HS 8C/16T, Radeon 680M,
21,245,952,000 bytes RAM, MSVC 19.44.35228, CMake 4.4.0, Ninja 1.13.2) and must not be generalised to other
hardware, models or drivers.

## Method

* Command: `HCSInfer benchmark --suite quick --model <gguf> --backend cpu|vulkan [--load-mode <m>] [--export <json>]`.
  `quick` is 1 warm-up, 3 measured iterations, 64 new tokens. `--suite full` adds a prefill sweep (TTFT and prompt
  tokens/s, 1 new token) and a decode sweep over `--sweep-sizes` (1..8 sizes of 16..1048576, default
  128,512,1024,2048); every point has its own median/p90 and samples, points that do not fit `--ctx` are `SKIPPED`,
  failed points make `result_status` `PARTIAL`. The same runner is reachable as `POST /hcs/v1/benchmarks` (a job).
  **No `full` run or API benchmark run on a real model has been recorded (NOT RUN); every table below is from the
  `quick` suite.**
* Output: one AGENTS.md 21.1 JSON document (`schema_version 1`): machine, build, model SHA-256, quantization,
  backend/device, context, cold and warm phases, median/p90 and the raw samples. Unmeasurable values are the string
  `unknown` or `not_applicable`.
* **"Cold" means process-cold, not page-cache-cold.** The OS file cache is not flushed (needs administrator rights), so
  every record says `"file_cache": "not_controlled"`. The Vulkan driver shader cache is also not controlled.
  Cold and warm numbers are never compared with each other, and different models, quantizations or prompts are never
  compared as if only the backend changed.
* Peak RAM is the process peak since start (it includes the cold phase and the warm reloads).
* The benchmark matrix IDs of AGENTS.md 16.6: B0 and B1 are measured (below), B7 is measured as one sample per
  configuration; **B2, B3, B4, B5, B6 and B8 are NOT RUN** (they need the Model Vault in the engine load path, HCS kernels
  or a storage-bound model; see `roadmap/STATUS.md`).

## B0 / B1: qwen3-0.6b Q4_K_M, CPU vs Vulkan

Source: `test-evidence/benchmark/README.md` and its JSON files. Git `fddc30df1218` (clean tree), both builds Release.
Model `qwen3-0.6b-q4_k_m.gguf`, 484,219,712 bytes, SHA-256
`4ea794120a3b2244a6e8a36354f1aa53e7f0c52b2f1f280dde0d7f522f37ae7c`, read from the external USB (NTFS) drive.
Identical parameters: context 4096, max-new-tokens 64, temperature 0, seed 42, 1 warm-up, 3 measured iterations,
one fixed 199-token prompt.

| Metric | B0 CPU | B1 Vulkan (Radeon 680M) |
|---|---|---|
| backend / device | `CPU` | `Vulkan`, `AMD Radeon(TM) Graphics (Vulkan0)`; API 1.4.315, driver `26.6.1` |
| cold model load | 502 ms | 546 ms |
| cold TTFT | 362 ms | 138 ms |
| cold decode | 66.7 tokens/s | 128.5 tokens/s |
| warm model load (median / p90) | 503 / 505 ms | 390 / 393 ms |
| warm TTFT (median / p90) | 369 / 387 ms | 134 / 142 ms |
| warm prompt processing (median / p90) | 558 / 577 tokens/s | 17,461 / 24,612 tokens/s (see caveat) |
| warm decode (median / p90) | 66.10 / 66.25 tokens/s | 126.14 / 126.50 tokens/s |
| peak process working set / commit | 1,159,835,648 B / 1,149,767,680 B | 304,033,792 B / 1,545,367,552 B |

Caveats from the same file: the very first Vulkan run on an empty driver shader cache recorded cold TTFT 2,810 ms, so a
cold Vulkan figure from an uncontrolled cache state must not be quoted as steady state. The lower Vulkan working set is
mostly UMA allocations that show up as commit, not a real memory saving. The Vulkan prompt-throughput field is
inconsistent with TTFT (asynchronous submission), so use TTFT for Vulkan prefill. `hdd_read_mib_s_median` is `unknown`
and `decompression_mib_s_median` is `not_applicable` because the Model Vault is not in the engine load path.

## LFM3-Omni-2B Q8_0 quick benchmark

Source: `test-evidence/lfm3/README.md` (the `-quiet` runs, one controlled run each; the other three files were recorded
while other processes were compiling and are kept for honesty). Same parameters as B0/B1 except a 223-token prompt.
Model `LFM3-Omni-2B-Q8_0.gguf`, 1,846,240,768 bytes, SHA-256
`59118f661eaacb93d204723d76355b53347f0940b508c11412f6290fd49158d4`.

| Metric (warm medians) | CPU, 8 threads | Vulkan (Radeon 680M) |
|---|---|---|
| decode tokens/s | 18.8 (p90 18.9) | 35.9 (p90 37.5) |
| TTFT | 2,790 ms | 438 ms |
| model load (warm) | 391 ms | 982 ms |
| peak process working set / commit | 2,057 MB / 528 MB | 514 MB / 2,790 MB |

## Load modes (M5 Stage A, CPU)

Source: `test-evidence/M5-loadmodes/README.md`. Same model and parameters as B0, `--no-hash`, build `build/img-api`
(Release, CPU-only), model read from the external USB drive. One run per mode, 3 measured iterations.

| mode | cold load ms | warm load median ms | warm decode t/s | peak working set MB | peak commit MB |
|---|---|---|---|---|---|
| `auto` | 630 | 664 | 60.8 | 1106 | 1096 |
| `mmap` | 674 | 651 | 60.2 | 1106 | 1096 |
| `none` | 768 | 655 | 59.7 | 984 | 1347 |
| `mlock` | 759 | 836 | 59.8 | 984 | 1347 |
| `mmap+mlock` | 900 | 871 | 59.3 | 1170 | 1097 |
| `direct_io` | 9538 | 9092 | 58.8 | 985 | 1348 |

Only the `direct_io` load-time effect (it bypasses the file cache; about 14x here) is large enough to survive the
uncontrolled cache state. Decode throughput does not differ because the model fits in RAM. A storage-bound comparison
(model larger than RAM, controlled cache) is BLOCKED on this machine.

## B7: stable-diffusion.cpp compute backend vs parameter backend

Source: `test-evidence/B7-params/README.md` and `b7-summary.json`. `sd-cli` of stable-diffusion.cpp `master-1106-532d2c1`
(pin `532d2c15c430a785a712b2d94281f04e733008a4`, patched GGML build), model SD1.5 Q8_0 (1,763,578,176 bytes) on the
internal `D:` drive, 256x256, 2 steps, seed 42, **one sample per configuration** (not medians).

| compute backend | params backend | wall ms | peak working set MB | peak commit MB |
|---|---|---|---|---|
| `vulkan0` | `cpu` | 5218 | 1772.8 | 3572.3 |
| `vulkan0` | `disk` | 3288 | 624.8 | 2255.0 |
| `cpu` | `cpu` | 9676 | 2219.5 | 2410.0 |

The output PNGs of the three runs differ byte-wise; equivalence is not verified. Disk parameter placement was faster
here only because the model sat on fast local storage and in the file cache; a USB-HDD run is NOT RUN, and the
ranking would very likely invert there.

## Earlier provisional numbers

`test-evidence/M1/llama/` (llama-bench, uncontrolled, few samples; for example qwen3 0.6B Q4_K CPU pp128 416.65 t/s,
tg64 67.89 t/s at build `d3c3e24`) and `test-evidence/M6-storage/` are not comparable with the controlled runs above.
