# Overload protection (plan F1) and the stress harness (plan F2 skeleton)

HCSInfer shares RAM with the iGPU on the target machine (21 GB, UMA). A model load, a large image or a burst of
requests can push the whole PC into swap. The overload controller keeps the runtime inside a configurable memory
envelope and gives you a way out when it is not enough.

Everything below is implemented in `src/scheduler` (`AdmissionController`) and `src/telemetry` (`ResourceProbe`),
wired in by `hcs::api::ControlPlane`. Engines never see it. Tests: `tests/unit/scheduler`,
`tests/contract/api_overload`, `tests/unit/core/test_job_queue_cancel_all.cpp`, `tools/stress/test_stress.py`.

## What is measured

| Figure | Source | Used for |
|---|---|---|
| free physical RAM | Windows `GlobalMemoryStatusEx` (`ullAvailPhys`), Linux `/proc/meminfo` `MemAvailable` | RAM floor, hard floor |
| commit charge / limit | Windows `GetPerformanceInfo` (`CommitTotal`/`CommitLimit`), Linux `Committed_AS`/`CommitLimit` | commit limit |
| process working set | `GetProcessMemoryInfo`, `/proc/self/statm` | reported only |
| GPU heap headroom | DXGI `QueryVideoMemoryInfo` (local + non-local budget - usage), best hardware adapter | GPU check for Vulkan loads / images |

The GPU figure is the **OS adapter budget (DXGI)**, a proxy: it is not `VK_EXT_memory_budget` of a Vulkan device
(creating a Vulkan device for every sample would be too heavy). Any figure the OS does not give is reported as the
string `"unknown"` and disables only the rule that needs it; nothing is guessed. A probe that throws counts as unknown.

## Admission control

Every inference job (text, vision, image), benchmark and model-load job takes a **ticket** before it is queued. The
ticket is released when the job closure is destroyed (finished, failed, cancelled or never started).

A request is refused with a structured error when, in this order:

1. level is **critical** (free RAM below the hard floor or commit above the hard limit): `overloaded`
2. level is **pressure** (free RAM below the soft floor or commit above the soft limit, latched with hysteresis):
   `overloaded`
3. `max_in_flight` jobs (running + queued) are already admitted: `overloaded`
4. the predicted need would break the floor: `out_of_memory` (free RAM - need < floor, commit after > limit, or GPU need >
   GPU heap headroom). The message carries remediation text (unload other models, reduce context or image size, close
   applications, use CPU mode).

Model loads additionally call `check_memory` inside the load with weights + KV-cache estimate, crediting what
unloading the currently loaded model gives back. Job working sets are heuristics (`estimate_text_job_bytes`,
`estimate_vision_job_bytes`, `estimate_image_job_bytes`; deliberately upper-ish, pure, unit-tested); model weights are
accounted at load time.

Defaults (`AdmissionConfig`): soft floor 2560 MiB free, commit < 90 %, hard floor 1024 MiB, hard commit 97 %,
hysteresis 512 MiB / 2 %, `max_in_flight` 4 (one worker, so queue length 3), `Retry-After` 5 s, watchdog every 1 s,
shed deadline 10 s. `serve` flags: `--min-free-ram-mb`, `--hard-floor-mb`, `--max-commit-pct`, `--max-active-jobs`,
`--watchdog-ms`, `--no-overload-protection`. Inconsistent values (hard floor above soft floor ...) are rejected.

**Hysteresis.** A level is left only when free RAM is `hysteresis` above its threshold (and commit 2 % below), so a
reading hovering around the floor does not flap between admit and refuse (tested with an oscillating probe).

## Backpressure on every surface

| Surface | saturated / overloaded | not enough memory |
|---|---|---|
| HTTP | `503` `overloaded` + `Retry-After: <s>`; the older in-flight cap and a full job queue stay `429` `too_many_requests`/`busy` + `Retry-After` | `503` `out_of_memory` + `Retry-After` |
| MCP | tool error with `error.code = "overloaded"` | `"out_of_memory"` |
| CLI | exit code 15 (`ErrorCode::Overloaded`) | exit code 5 (`ErrorCode::OutOfMemory`) |

The HTTP health route, job cancellation, `GET /hcs/v1/runtime/overload` and `POST /hcs/v1/runtime/shed` are never
throttled: cancelling and shedding are the way out of an overload.

## Kill switch and watchdog

`POST /hcs/v1/runtime/shed` (bearer token required) or the MCP tool `hcs.runtime.shed` runs `ControlPlane::shed`:

1. `JobQueue::cancel_all()` cancels every queued job and signals every running one,
2. the model is unloaded on a helper thread, 3. both are bounded by the **shed deadline** (default 10 s).

The report says exactly what happened: `jobs_cancelled`, `model_unloaded`, `unload_pending` (an engine call that cannot
be interrupted, e.g. a weight read in progress, is still running; it is joined at shutdown), `jobs_idle` (every
cancelled job reached a terminal state in time), `duration_ms`. A load whose token is cancelled **while the engine was
loading** is torn down again instead of being published.

The **watchdog** thread (started by `Runtime::create`) polls every `watchdog_interval`; on entering the critical level
(free RAM below the hard floor, or commit above the hard limit) it runs the same kill switch **once per episode** and
records it in `GET /hcs/v1/runtime/overload` (`last_shed`, `counters.shed_count`). While critical, admission refuses
everything; it recovers with hysteresis.

Limits of this design (honest list): the controller reacts to what the OS reports at one-second resolution; an engine
that allocates several GB in one call can still overshoot before the watchdog sees it (the predicted-need check at
admission is the first line of defence); Windows may page memory regardless of these checks; the GPU figure is a DXGI
proxy; no CPU-% or temperature rule exists yet.

## Stress harness (`tools/stress/stress.py`)

Standard-library Python (no venv or pip needed; run isolated). It ramps **context length**, **image size** and
**client concurrency** step by step against a running `HCSInfer serve`, polls memory through the controller
(`GET /hcs/v1/runtime/overload`, falling back to a local OS probe, and refusing to ramp blind), and **aborts** on:

`free_ram_below_floor`, `commit_above_limit`, `server_critical`, `error_rate`, `latency_spike`, `process_gone`
(`--server-pid`, or 3 failed polls), `runtime_limit`, `c_drive_growth` (free space on C: shrank), `no_memory_visibility`.

HTTP 429/503 answers are backpressure, not errors; a step where more than `--max-refused-rate` of the requests were
refused ends that axis with `limit_reached`. On abort the harness pulls the kill switch (unless `--no-shed-on-abort`)
and writes a JSON report (schema version 1: config, limits, per-step status counts / latency median+p90 / memory
before-min-after, abort reason and step, shed report, memory timeline).

```powershell
. .\scripts\env-d-e.ps1                      # nothing on C:
$env:HCS_API_TOKEN = '<the token printed by serve>'
python -I tools\stress\stress.py --model <model-id> --dry --report D:\HCSScratch\stress.json
python -I tools\stress\stress.py --model <model-id> --axes context,concurrency,image --floor-mb 3000
python -I tools\stress\test_stress.py        # self-test against a fake server (no HCSInfer, no model)
```

The token is read from the environment only (never argv), never printed and never written to the report. This is the
F2 *skeleton*: the Batch/thread sweep, per-run AGENTS.md 21.1 benchmark JSON and the GPU/temperature abort rules are not
implemented yet.
