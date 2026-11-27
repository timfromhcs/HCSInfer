# S3 evidence: full soak, fault injection, fuzz and XSS (2026-11-27)

Branch `release/v1.1-stable`, commit `35c3bd6` (working tree also carried in-progress docs), CPU build
`build/windows-msvc-release`, real LFM3-Omni-2B Q8_0 + mmproj and SD1.5 Q8_0 from `D:\HCSModels`, temp on D:,
free RAM logged as 11.54 GB before the run, `realmodel.lock` held throughout.

| Gate | Status | Evidence |
|---|---|---|
| Full soak profile (`scripts/soak.ps1 -Profile full`) | **PASS** | `soak-full.txt` and `soak-full/summary.json`: **13/13 phases PASS** - 100 load/unload cycles (118.8 s), 20 model switches (25.8 s), 30 vision runs (61.3 s), 20 diffusion runs (177.9 s), 60 cancel rounds (19.8 s), 3 shutdown phases (8.3/15.7/35.4 s), 30-minute continuous generation (1807.3 s), 100 unplug-injection rounds in 3 phases (10/59.5/14.8 s), HttpStorm 40 rounds (63.1 s). Exit 0. |
| Leak / resource growth | **PASS** | `soak-full/soak-core.jsonl`: 100-cycle load/unload private growth **41.0 MiB** (limit 128), handle growth **0**, thread growth -1; 256-sample continuous generation private growth **5.68 MiB**, handle growth -1. All leak verdicts PASS. |
| Throughput drift over 30 minutes | **PASS** | 256 samples; first fifth **18.97 t/s**, last fifth **19.09 t/s**, drift **+0.63%** (raw samples in `soak-core.jsonl`). |
| HttpStorm | **PASS** | `soak-full/http-storm.json`: **18/18 checks** - streaming FIN/RST disconnect storm, abandoned requests, 64 idle/half-open sockets, unload/reload during a live stream, hard kill + restart, Ctrl+Break while generating. |
| Parser fuzz smoke (UBSan, no ASan) | **PASS (smoke)** | `libfuzzer-smoke.txt`: 376,006 executions / 61 s, 0 crashes, 0 UBSan reports, peak RSS 30 MB; built with ROCm clang 21, `-fsanitize=fuzzer,undefined`. ASan not available with this CRT; long campaign NOT RUN. |
| Runtime XSS regression | **PASS** | `xss/xss-run.txt`: 9/9 checks with a real headless browser and a hostile model reply; no script/img/handler/CSP violation. Previously skipped because `websockets` was missing. |

## Limits

Physical USB unplug, true RAM exhaustion and a GPU Vulkan soak remain NOT RUN; fault injection and the CPU soak are
the documented substitutes. No long fuzz campaign or ASan run.
