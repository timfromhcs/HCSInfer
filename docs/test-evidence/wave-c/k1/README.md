# wave C / k1: upstream GGML Vulkan baseline vs HCS Vulkan kernels

Machine: AMD Radeon 680M (`vk:1002:1681`, driver 26.6.1, Vulkan 1.4.315, subgroup 64), Windows 11, Release,
`windows-msvc-release-vulkan`-style build in `D:\HCSInfer\build\k1`, source `1469638` + k1 work (`+dirty` in the JSON).
All GPU runs held `D:\HCSScratch\realmodel.lock` (free RAM ~10 GB logged in `gpu-run.log`).

| file | what | status |
|---|---|---|
| `ggml_vk_baseline_tests.txt` | `hcs_test_ggml_vk_baseline`, 7 tests on the real device | PASS 7/7 |
| `vulkan_ops_tests.txt` | `hcs_test_vulkan_ops` (kernels vs oracle, device-lost + dispatch-timeout recovery, `TuneCli.*` incl. `--reset`), 20 tests | PASS 20/20 |
| `ggml-vk-bench-llm.json/.txt` | upstream ggml Vulkan operator timings, suite llm, 15 samples | PASS (one op `error`: F32 1024x1024x64 spot check) |
| `prefix-tune-llm-run1.json/.txt` | one complete `tune --suite llm` run with the **pre-fix** binary (exit 4: 5 GEMM buckets at k=4096 `FAIL` with unit-range data) | valid for the other 59 buckets |
| `decision-single-run.json/.md`, `single-run-table.md` | `scripts/kernel-decision.ps1` / per-bucket table from that run: 0 promoted | single run, provisional |
| `gpu-run.log`, `lock-timeout.txt` | lock acquisition log; second attempt timed out after 45 min twice | |

NOT RUN / BLOCKED: tune runs 2 and 3 (and any run with the k>1024 operand-scale fix and the added LayerNorm model
shapes): the lock file was an orphaned 0-byte file created 21:34:52 by another agent's wrapper (its script has a
`[Text]::Encoding` typo that creates the file and then fails before writing), with no process holding it and no
real-model process running; it was not removed because it is not this task's lock. Full `ctest` NOT RUN.
