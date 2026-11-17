# `HCSInfer benchmark --suite quick` - first real run (B0-text, CPU)

Date: 2026-11-17. Machine: Windows 11 build 10.0.26200.9974, AMD Ryzen 7 7735HS (8C/16T), Radeon 680M,
21,245,952,000 bytes RAM installed. Build: `build/img-api`, CMAKE_BUILD_TYPE=Release, `HCS_ENABLE_VULKAN=OFF`,
compiler MSVC 19.44.35228 x64, CMake 4.4.0, Ninja 1.13.2, git `d5baf76539d3+dirty` (recorded at configure time).
Driver version string from the doctor report: 32.0.21043.12001 (Vulkan paths NOT RUN in this build).

Model: `qwen3-0.6b-q4_k_m.gguf`, GGUF, quantization `q4_k_m` (**taken from the file name**, flagged as such in the
JSON), 484,219,712 bytes, SHA-256 `4ea794120a3b2244a6e8a36354f1aa53e7f0c52b2f1f280dde0d7f522f37ae7c`, read from
`E:` (USB, NTFS) - the external drive of the Model Vault design, not the SSD.

Command:

```
build\img-api\bin\HCSInfer.exe benchmark --suite quick ^
  --model E:\AILOCALTRAIN\data\gguf\qwen3-0.6b-q4_k_m.gguf --backend cpu ^
  --export docs\test-evidence\benchmark\benchmark-quick-cpu-qwen3-0.6b-q4_k_m.json
```

Parameters: context 4096, max-new-tokens 64, temperature 0 (greedy), seed 42, warm-up 1, measured iterations 3, one
fixed prompt of 199 tokens (the real prompt token count is in the JSON; the prompt text itself is not logged).

## Files

| File | Content |
|---|---|
| `benchmark-quick-cpu-qwen3-0.6b-q4_k_m.json` | the AGENTS.md 21.1 document (`schema_version 1`) exactly as the tool wrote it (same bytes as the stdout capture) |
| `benchmark-quick-cpu-qwen3-0.6b-q4_k_m.stdout.json` | stdout of the same run |
| `benchmark-quick-cpu-qwen3-0.6b-q4_k_m.stderr.log` | the human-readable per-iteration progress lines |

## Measured values (CPU, not a comparative benchmark)

| Metric | cold (first load + first generation) | warm (median of 3) | warm p90 |
|---|---|---|---|
| model load | 525 ms | 480 ms | 506 ms |
| time to first token | 400 ms | 356 ms | 378 ms |
| prompt processing | 514 tokens/s | 579 tokens/s | 600 tokens/s |
| decode | 67.1 tokens/s | 66.6 tokens/s | 66.7 tokens/s |
| wall time per generation | - | 1305 ms | - |
| peak process working set | 1,159,467,008 B (peak commit 1,149,464,576 B) | | |

## Honesty notes / limitations

* **`cold` means process-cold, not page-cache-cold.** The OS file cache was not flushed (that needs administrator
  rights); the JSON says `"file_cache": "not_controlled"` next to every record. The model was read from the external
  USB drive, but a warm Windows cache can still have served it.
* `hdd_read_mib_s_median` is `"unknown"` here (the doctor read-benchmark is not part of this run);
  `decompression_mib_s_median` is `"not_applicable"` (no compression in the load path yet, M5/M6 not wired).
* `offloaded_layers = 0`, `total_layers = 28`, backend `CPU`: **this is a CPU-only run**. The equivalent Vulkan run
  (`benchmark_id B1-text`) has not been recorded yet - the CPU build has no Vulkan engine.
* Peak RAM is the process peak working set since start, so it includes the cold phase and the warm reloads (the JSON
  says so in `peak_ram_note`). It is not a per-phase figure.
* One model, one quant, one context length, one prompt, one machine: no claim about other models or hardware.
* Numbers must not be compared against the earlier provisional M1 numbers: different methodology
  (`docs/decisions/ADR-010-benchmark-methodology.md`), different sampling and no controlled file cache.
