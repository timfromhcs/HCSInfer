# Soak defects (found by the ported soak harness on `main` 902b41e + the uncommitted W2 port, 2026-11-18)

Run: `scripts/soak.ps1 -BuildDir build/rel -ModelRoot D:\HCSModels -Profile quick -Label cpu-quick` on the target
machine (Ryzen 7 7735HS, CPU-only Release build, LFM3-Omni-2B Q8_0 + mmproj and SD1.5 Q8_0, models read-only).
Evidence: `cpu-quick/` (`summary.json`, `run-meta.json`, `soak-core.jsonl`, one `.log`/`.gtest.json` per test).
This is the **quick** profile only (8 load/unload cycles, 60 s generation, ...); the **full** profile (100 cycles,
30 min generation) is NOT RUN.

Result of the final run: 12 gtest cases PASS (3 fault-injection, load/unload, model switch, vision, diffusion,
cancel storm, 3 shutdown cases, 60 s continuous generation), `HttpStorm` FAIL (2 of 18 checks). Nothing below is
fixed in this change; the harness reports them, it does not hide them.

## D-S1 - destroying `api::Runtime` while a job is running crashes (access violation)

* Seen in the first run: `Soak.ShutdownWhileLoading`, `ShutdownWhileGenerating`, `ShutdownWhileRenderingImage` all
  ended with exit code `-1073741819` (0xC0000005) after 4-8 s. (That first run's logs were overwritten by the
  final run; the exit codes are quoted from the console of the first run.)
* Reproducer: in `tests/soak/soak_core/test_soak_core.cpp` `destroy_with_watchdog`, remove the two
  `queue->shutdown()` / `engine_queue->shutdown()` calls so the `Runtime` is destroyed directly.
* Cause hypothesis (not proven with a debugger): `Runtime` declares `queue` before `control`, so `control` is
  destroyed first while a worker of `queue` is still running a load/chat job that uses it. `serve` and `mcp stdio`
  avoid this by calling `queue->shutdown()` and `engine_queue->shutdown()` before the `Runtime` goes out of scope
  (`src/api/runtime.cpp`, `src/mcp/stdio.cpp`); the soak tests now follow the same protocol and then PASS, so the
  product's shutdown path is fine, but the destructor is not safe on its own. Fix direction: join both queues in
  `~Runtime()` before any member is destroyed.

## D-S2 - HttpStorm C: health stops answering while ~64 idle / half-open connections are held

* `docs/test-evidence/soak/cpu-quick/http-storm.log`: `FAIL C: health still answers while ~64 idle/half-open
  connections are held -- WinError 10053` (the server aborted the new connection). The server survived and
  recovered promptly once the flood was released (the next two checks PASS).
* Hypothesis: the connection cap (`--max-inflight`, default 8) is consumed by idle sockets that the read timeout has
  not yet reclaimed, so a local client can starve the unauthenticated `GET /hcs/v1/health`. Loopback-only, so a
  local denial of service, not a remote one. Not triaged further.

## D-S3 - HttpStorm F: `Ctrl+C` during generation does not stop `serve` within 60 s

* `FAIL F: server exits on Ctrl+C while generating -- still running after 60 s`. It is not known whether the
  server ignores the console control event while a generation runs (shutdown waits for the job) or whether the
  harness's `CTRL_C_EVENT` delivery to a process in another process group does not arrive. NOT triaged; treat as an
  open defect until a manual `Ctrl+C` run settles it.

## Harness change made while porting

`http_storm.py` now runs `HCSInfer models scan <root>` into its private state directory before `serve`: on `main`,
`serve` only loads the persisted registry, so without the scan `/v1/models` is empty and every later check fails.
