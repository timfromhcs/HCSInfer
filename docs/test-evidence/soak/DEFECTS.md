# Soak defects (found by the ported soak harness, 2026-11-18) and their status after wave 3

Original run: `scripts/soak.ps1 -BuildDir build/rel -ModelRoot D:\HCSModels -Profile quick -Label cpu-quick` on the
target machine (Ryzen 7 7735HS, CPU-only Release build, LFM3-Omni-2B Q8_0 + mmproj and SD1.5 Q8_0, models read-only)
against `main` 902b41e + the uncommitted W2 port. Evidence: `cpu-quick/`. 12 gtest cases PASS, `HttpStorm` FAIL
(2 of 18 checks).

Wave 3 re-run (same profile, `5633450` + the wave-3 working tree, uncommitted): `cpu-quick-w3/` (12 gtest cases
PASS, now WITHOUT the shutdown workaround) and `cpu-quick-w3-httpstorm-rerun/` (HttpStorm 18/18 PASS, see D-S2
and D-S3). The **full** profile (100 cycles, 30 min generation) is NOT RUN.

| ID | Status | One line |
|---|---|---|
| D-S1 | **FIXED** (red -> green, real engines re-verified) | destroying `api::Runtime` with a running job crashed |
| D-S2 | **FIXED** (red -> green, real server re-verified) | ~64 idle/half-open sockets made `GET /hcs/v1/health` fail |
| D-S3 | **Harness problem (original symptom) + one genuine latent defect FIXED** | `Ctrl+C` during generation |
| D-S4 | **FIXED** (new, found by the wave-3 transport tests) | slowloris: no overall request deadline |

## D-S1 - destroying `api::Runtime` while a job is running crashed (access violation) - FIXED

* Seen first as `Soak.ShutdownWhileLoading/Generating/RenderingImage` exiting `0xC0000005`.
* Cause (confirmed): `Runtime` declares `queue` before `control`, so `control` (and then the engines) were destroyed
  while a worker of `queue` still ran a job that used them. The queues were only joined when their own members were
  destroyed, i.e. last.
* Reproduced RED first: new contract test `RuntimeLifecycle.DestroyWithRunningChatJobDoesNotCrash`
  (`tests/contract/api/test_runtime_lifecycle.cpp`, fake engine blocking until cancelled) crashed the test binary
  (exit 139 / access violation) before the fix.
* Fix: `Runtime::~Runtime()` (`src/api/runtime.cpp`) shuts down and joins both queues first, then drops `control`;
  non-copyable. `queue->shutdown()` is idempotent, so the `serve` / `mcp stdio` protocol is unchanged and still
  valid (`RuntimeLifecycle.ExplicitShutdownThenDestroyStillWorks`).
* Verified: 4 `RuntimeLifecycle` tests green; the soak shutdown tests no longer call `queue->shutdown()` themselves
  (workaround removed from `destroy_with_watchdog` only after the fix was proven) and PASS with the real engines
  (`cpu-quick-w3/Shutdown*.log`: loading 8.6 s, generating 10 s, rendering 16.7 s).

## D-S2 - HttpStorm C: health stops answering while ~64 idle / half-open connections are held - FIXED

* Cause (confirmed): `serve` ran with 10 connection workers + 16 queued connections; 64 held sockets exhausted that
  budget, so the accept loop closed the health connection (`WinError 10053`). Workers only block on socket reads
  until the handler's `--max-inflight` limit applies, so the small pool protected nothing.
* Reproduced RED first: `ApiSocketAudit.ServeBudgetKeepsHealthAnsweringUnder64HeldConnections` (real sockets, 32
  idle + 32 half-written POSTs, ONE health attempt) failed with the old 10 + 16 budget.
* Fix: `serve_transport_options()` (`src/api/runtime.cpp`): at least 48 workers (`max-inflight + 40`) + 32 queued,
  first-byte timeout 2 s, read timeout 5 s. Loopback/auth/limit semantics are unchanged (the inflight cap still
  yields `429`; connections beyond pool + queue are still closed at accept time).
* Honest limit: a pool is finite. A local client holding more than ~80 sockets still exhausts it (the flood is then
  refused at accept time and recovers as soon as it is released, `ApiSocketAudit.IdleConnectionFloodDoesNotStarve...`).
  There is no per-source quota because every local client has the same address.
* Re-verified on the real server: `cpu-quick-w3-httpstorm-rerun/http-storm.log` C: 3/3 PASS. A side effect of the
  bigger pool is a one-time rise of ~93 kernel handles when every worker has served its first connection; the
  harness now warms the pool before taking its baseline (the first wave-3 HttpStorm run failed ONLY the
  handle-growth check `93 > 48` for this reason). A separate probe (`handle_probe`: 6 floods of 70 connections)
  stayed flat at 221 handles after the first flood, so it is pool warm-up, not a leak.

## D-S3 - HttpStorm F: `Ctrl+C` during generation does not stop `serve` within 60 s - harness problem + latent defect fixed

Triage, with evidence (`w3-ctrl-c/ctrl-c-manual.txt`, tool `tests/soak/http_storm/ctrl_c_manual.py`):

1. **The original symptom was a harness/environment problem.** A harness started from a shell that disabled Ctrl+C
   passes the "ignore Ctrl+C" console flag to its children, so `os.kill(0, CTRL_C_EVENT)` was silently dropped. In
   a manual run with that flag inherited, the server never even logged `shutting down` (`STILL RUNNING after
   60 s`, no log line), i.e. the process never received the signal; it was not a slow shutdown.
2. With the flag cleared (real `CTRL_C_EVENT` delivered to the server's own console via `AttachConsole` +
   `GenerateConsoleCtrlEvent`, real LFM3 model streaming): the server exited with rc=0 in 0.4 s and logged
   `shutting down`.
3. **A genuine latent defect was found while triaging and fixed:** `run_serve` stopped the HTTP transport first.
   `HttpServer::stop()` joins every connection thread, so a request thread blocked on a running generation kept
   shutdown waiting until the generation ended or `--timeout-s` (default 300 s) expired. Reproduced RED:
   `ApiSocketAudit.GracefulShutdownCancelsRunningGenerationPromptly` hung > 15 s. Fix: new
   `graceful_shutdown(server, runtime)` cancels and joins the job queues first, then stops the transport; `serve`
   also handles `SIGBREAK` (Ctrl+Break / console close on Windows). The test now completes in ~70 ms.
4. Harness: phase F starts the server in its own process group and sends `CTRL_BREAK_EVENT` (not subject to the
   inherited ignore flag): `F: server exits on Ctrl+Break while generating (rc=0, 0.3 s)` PASS. The real
   `CTRL_C_EVENT` path is covered by the manual tool above (run once, not part of the automated harness).

## D-S4 - slowloris: no overall deadline for receiving a request - FIXED (new)

* Found by `TransportNegative.SlowlorisHeaderDripIsCutOffInBoundedTime` (raw socket dripping 1 header byte per
  600 ms against a 2 s per-read timeout): the connection stayed open for the full 16 s test window. cpp-httplib only
  has per-read timeouts that reset on every byte, so one client could hold a worker indefinitely.
* Fix: `TransportOptions::request_deadline_s` (default 15 s) - a reaper thread shuts down any connection that has
  been receiving a request (headers + body) longer than the deadline; the clock stops while a request is being
  handled/streamed and restarts when the response is complete. Needs `process_and_close_socket` to be callable from
  a subclass: `cmake/HcsHttplib.cmake` builds a copy of `httplib.h` with that one member `private` -> `protected`
  (documented in `external/dependency-lock.json`; configure fails if the declaration moves).
* Regression-guarded: `TransportNegative.LongRunningHandlerAndStreamAreNotKilledByTheRequestDeadline` (2 s
  generations and a stream with a 1 s deadline are NOT cut).
* Known residual: the reaper's `shutdown()` on a socket races, in a microsecond window, with httplib closing that
  same socket when the connection ends on its own at the exact moment the deadline expires; the worst case is one
  extra reset of a connection that was ending anyway.

## Harness changes made while porting / in wave 3

* `http_storm.py` runs `HCSInfer models scan <root>` into its private state directory before `serve` (on `main`,
  `serve` only loads the persisted registry).
* Wave 3: phase F uses its own process group + `CTRL_BREAK_EVENT`; a connection-pool warm-up precedes the baseline
  sample (see D-S2).
