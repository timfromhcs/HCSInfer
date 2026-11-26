# Wave B / e1 evidence (Model Vault in the load path, CLI load/unload/status, streaming, fast start)

Machine: Windows 11, Ryzen 7 7735HS, 21 GB shared RAM, models on `D:\HCSModels` (D: is a USB-attached external drive,
bus type usb per `HCSInfer doctor`; a second USB drive E: exists and was NOT measured). Build: MSVC 19.44 Release, CPU
backend only (no Vulkan), `D:\HCSInfer\build\e1`, branch `wave-b/e1`. External sources (`external/llama.cpp`,
`external/stable-diffusion.cpp`) were copied from the main checkout's working tree (same pins; not re-verified by
`git submodule status` because submodule init is blocked in this worktree).

## Files
- `vault-load-usb-d.json` - `tools/benchmark-runner/vault_load_bench.py`, 12 real `serve` runs (N=3 per cell), cold = file
  cache evicted without admin rights (verified 2261 -> 45.7 MiB/s), raw per-run values, `io_write_bytes` per process.
- `e2e/e2e-real.json` - real exe + real SmolVLM2-500M Q8_0: HTTP, SSE, cancel, **client disconnect**, **CLI load/unload/status**,
  **vault phase**, MCP (see PASS/FAIL in the file).

## Start-up and scan (this machine, same binary)
| measurement | before | after |
|---|---|---|
| `HCSInfer --version` | 19-38 ms | unchanged |
| serve: process start -> "listening" | 20-130 ms (`startup_s`) | unchanged |
| `models scan D:\HCSModels` (37k files, venv inside), warm dir cache, x3 | 3597 / 3617 ms | 418-446 ms (x5) |
| same, first ever (cold directory cache) | 18.9 s | not re-measurable |
| `models list` | 15 ms | unchanged |
The scan found the same 10 models/paths before and after. `models scan` never hashes. Cause of the slowness: every
directory entry was canonicalised even when it was no model candidate.

## Gates
| gate | status |
|---|---|
| unit/contract tests for the new code (VaultPrefetch 7, VaultLoad 9, CliRemote 11, Cli hash, registry digest/scan) | PASS (targeted runs) |
| vault cold load, real engine, USB drive, N=3 | PASS (1.7x faster cold, 2.2x slower warm; default stays OFF) |
| vault on second USB drive E: | NOT RUN |
| Vulkan backend with vault stage | NOT RUN |
| no staging file | PASS: process wrote <= 160 KiB per run (GetProcessIoCounters) and the e2e checks the model dir is unchanged |
| e2e real engine (SmolVLM2-500M Q8_0, CPU): 64 checks incl. SSE, cancel endpoint, client disconnect, CLI status/unload/load, vault phase, MCP | PASS 64/64 (`e2e/e2e-run.log`, `e2e/e2e-real.json`). Disconnect: job `cancelled` 0.03 s after the client died, no job pending, chat works afterwards. CLI load 0.46 s. Vault phase: digest verified against `models hash`, buffers 24 MiB, model dir unchanged |
| load test with 50 parallel SSE clients (plan E3) | NOT RUN |
| SSE backpressure beyond the existing unit test | NOT RUN |
| e2e with a first run that failed | the first attempt picked the sibling mmproj as "the model" (test helper bug, fixed to prefer the exact id); no product defect |
