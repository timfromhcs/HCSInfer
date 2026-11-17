# `HCSInfer benchmark --suite quick` - first real runs (B0 CPU, B1 Vulkan)

Date: 2026-11-17. Machine: Windows 11 build 10.0.26200.9974, AMD Ryzen 7 7735HS (8C/16T) with Radeon 680M,
21,245,952,000 bytes RAM installed, external USB (NTFS) drive as the model source. Compiler MSVC 19.44.35228 x64,
CMake 4.4.0, Ninja 1.13.2, both builds `CMAKE_BUILD_TYPE=Release`, git `fddc30df1218` (clean tree at configure time,
recorded in the JSON as `git_commit`).

Model for both runs: `qwen3-0.6b-q4_k_m.gguf`, GGUF, quantization `q4_k_m` (**from the file name**, flagged as such),
484,219,712 bytes, SHA-256 `4ea794120a3b2244a6e8a36354f1aa53e7f0c52b2f1f280dde0d7f522f37ae7c`.

Identical parameters in both runs: context 4096, max-new-tokens 64, temperature 0 (greedy), seed 42, warm-up 1,
3 measured iterations, one fixed prompt of 199 tokens (real token count from the engine; the prompt text is not
logged).

```
build\img-api\bin\HCSInfer.exe benchmark --suite quick --model <gguf> --backend cpu    --force --export <B0.json>
build\windows-msvc-release-vulkan\bin\HCSInfer.exe benchmark --suite quick --model <gguf> --backend vulkan --force --export <B1.json>
```

## Files

| File | Content |
|---|---|
| `benchmark-quick-cpu-qwen3-0.6b-q4_k_m.json` | B0-text, the AGENTS.md 21.1 document (`schema_version 1`) as written by the tool |
| `benchmark-quick-cpu-qwen3-0.6b-q4_k_m.stdout.json` | stdout of the same run (identical bytes) |
| `benchmark-quick-cpu-qwen3-0.6b-q4_k_m.stderr.log` | per-iteration progress lines |
| `benchmark-quick-vulkan-qwen3-0.6b-q4_k_m.json` / `.stdout.json` / `.stderr.log` | the same for B1-text (Vulkan) |

## Measured values

| Metric | B0 CPU | B1 Vulkan (Radeon 680M) |
|---|---|---|
| backend / device as reported by the engine | `CPU` | `Vulkan`, `AMD Radeon(TM) Graphics (Vulkan0)` |
| Vulkan API / driver | - | 1.4.315 / `26.6.1 (AMD proprietary shader compiler)` |
| layers offloaded | 0 of 28 | 29 of 28 (upstream log convention: llama.cpp counts the output layer on top of `n_layer`; `total_layers` is `llama_model_n_layer`) |
| cold model load | 502 ms | 546 ms |
| **cold** TTFT | 362 ms | 138 ms |
| cold prompt processing | 575 tokens/s | 9,056 tokens/s |
| cold decode | 66.7 tokens/s | 128.5 tokens/s |
| warm model load (median / p90) | 503 / 505 ms | 390 / 393 ms |
| **warm** TTFT (median / p90) | 369 / 387 ms | 134 / 142 ms |
| warm prompt processing (median / p90) | 558 / 577 tokens/s | 17,461 / 24,612 tokens/s |
| warm decode (median / p90) | 66.10 / 66.25 tokens/s | 126.14 / 126.50 tokens/s |
| warm wall time per 64-token generation | 1,321 ms | 636 ms |
| peak process working set | 1,159,835,648 B | 304,033,792 B |
| peak process commit | 1,149,767,680 B | 1,545,367,552 B |
| decode samples (3 runs) | 66.10, 66.25, 65.67 | 125.37, 126.50, 126.14 |

Read the two columns only as "same model, same prompt, same parameters, same commit, same machine, two different
HCSInfer builds": decode is ~1.9x and TTFT ~2.8x better on the iGPU, and prompt processing is an order of magnitude
faster.

## Honesty notes / limitations

* **`cold` means process-cold, not page-cache-cold.** The OS file cache was not flushed (that needs administrator
  rights); every record says `"file_cache": "not_controlled"`. The model lives on the external USB drive, but a warm
  Windows cache can still have served it.
* **The Vulkan driver's shader/pipeline cache is not controlled, and it matters.** The very first Vulkan run on this
  machine (right after the Vulkan build was produced, empty driver cache) recorded cold TTFT **2,810 ms**, cold prompt
  processing 72 tokens/s and cold decode 82.5 tokens/s; the run recorded here (driver cache warm) recorded 138 ms /
  9,056 tokens/s / 128.5 tokens/s. A cold Vulkan figure from an uncontrolled cache state must not be quoted as
  steady-state performance. The warm figures are the meaningful ones.
* **Peak working set vs commit.** Vulkan's working set is much smaller mainly because the weights live in
  GPU-visible (UMA) memory instead of normal process pages; the commit is *higher* (1.55 GB vs 1.15 GB). On this APU
  CPU and GPU share the same physical RAM, so this is not a "Vulkan uses 850 MB less memory" claim.
* `hdd_read_mib_s_median` is `"unknown"` and `decompression_mib_s_median` is `"not_applicable"`: the Model Vault
  pipeline is not in the engine load path yet (M5), so there is nothing to measure there.
* Peak RAM is the process peak since start and therefore includes the cold phase and the warm reloads (the JSON says
  so in `peak_ram_note`).
* One model, one quantization, one context length, one prompt, one machine, 3 measured iterations. This is a baseline
  pair, not a performance matrix, and no claim is made about other models or hardware.
* Must not be compared with the earlier provisional M1 numbers: different methodology, different sampling, no
  controlled file cache (`docs/decisions/ADR-010-benchmark-methodology.md`).
