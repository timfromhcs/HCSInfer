# Soak and hardening tests (AGENTS.md 16.1G, M10)

Real-model reliability tests. No mocks: the in-process tests build the same `api::Runtime` that `HCSInfer serve`
builds (real llama.cpp text/vision engine, real stable-diffusion.cpp engine, real control plane, job queue and
memory budget) and run it against LFM3-Omni-2B Q8_0 (+ mmproj) and SD1.5 Q8_0.

## Running

```powershell
# build (see build recipe in docs/build.md); targets: hcs_test_soak_core, hcsinfer
# (any build directory works: -BuildDir build/rel)
scripts/soak.ps1 -BuildDir build/soak -ModelRoot D:\HCSModels -Profile quick -Label cpu
scripts/soak.ps1 -BuildDir build/soak -Profile full  -Label cpu     # 100 cycles, 30 min generation, ...
scripts/soak.ps1 -Only LoadUnloadCycles,HttpStorm                    # subset
```

Evidence lands in `docs/test-evidence/soak/<label>/`: `soak-core.jsonl` (one JSON object per sample / verdict),
`<Test>.log`, `<Test>.gtest.json`, `http-storm.json`, `summary.json`, `run-meta.json`.
`hcs_test_soak_core` is always built next to the other tests, but its tests are **not registered with CTest by
default** (the default `ctest` stays fast and can never show a soak as passed). Configure with
`-DHCS_SOAK_REGISTER=ON` to register them under the label `soak` (`ctest -L soak`); they run the same gtests as a
quick smoke when `HCS_SOAK_MODEL_ROOT` is set. Without it every test that needs a model is reported `NOT RUN`
(skipped with a message), never a pass. A real soak run is NOT RUN unless the evidence directory below says it was
executed; the file existing in the tree is not evidence.

## What is tested

| Test | What it proves |
|---|---|
| `Soak.LoadUnloadCycles` | N x load -> (every 10th: real 8-token generation) -> unload through `ControlPlane`; budget returns to 0 each cycle; private bytes / handles / threads do not grow |
| `Soak.ModelSwitchCycles` | text -> image -> unload loop (switching frees the previous model) |
| `Soak.ContinuousGeneration` | back-to-back 128-token generations for `HCS_SOAK_GEN_SECONDS`; decode tokens/s of the first fifth vs last fifth of the run (drift), TTFT, memory per iteration |
| `Soak.VisionRepeat` | repeated image understanding with the paired mmproj |
| `Soak.DiffusionRepeat` | repeated 256x256 SD1.5 generations, output must be a PNG |
| `Soak.CancelStorm` | bursts of 3 jobs cancelled at pseudo-random moments (running and queued); all must terminate Cancelled/Succeeded, engine usable afterwards |
| `Soak.ShutdownWhile{Loading,Generating,RenderingImage}` | the whole `Runtime` is destroyed mid-load / mid-generation / mid-render on a watchdog thread (hang = failure) |
| `Soak.UnplugGgufParseFaultInjection` | a `ByteSource` that fails after N bytes (device removed) under the real GGUF parser: clean `IoError`/`ModelError`, no throw |
| `Soak.UnplugTruncatedHcsModelStream` | an HCSMODEL container cut at 7 offsets (drive vanished mid-copy): every cut is a clean error, never partial success; intact file still streams |
| `Soak.UnplugEngineLoadTruncatedAndVanishedFile` | engine load of a 32 MiB-truncated GGUF copy and of a registered file deleted afterwards: clean failure, lifecycle + budget rolled back, engine usable for the next good model |
| `HttpStorm` (`tests/soak/http_storm`) | against a real `serve` process: streaming-disconnect storm (FIN and RST), abandoned non-streaming requests, 64 idle/half-open connections, unload/reload while a stream is live, hard kill mid-stream + restart on the same state dir, Ctrl+C while generating |

### HDD-unplug simulation: scope and honesty

The real USB drive is never disconnected or written. Fault injection is used instead:

* the `ByteSource` seam is the only place the engine-independent code reads a model through an interface, so
  `parse_gguf` is exercised under injected read failure;
* a truncated container/copy has the same on-disk effect as a drive that vanished mid-copy;
* the llama/sd.cpp loaders read the model file themselves (mmap), so a read fault *inside* a running engine load
  (page-in failure of an already-mapped file) cannot be injected without removing the device and is **NOT RUN**.
  A mapped file whose device disappears mid-inference is a known OS-level hazard (SEH in-page error); it is
  listed in the known-limits section of the soak report.

## Leak thresholds (documented, overridable)

Compared as *first sample after warm-up* vs *last sample*; warm-up is 1-3 samples depending on the test.

| Quantity | Default limit | Env override |
|---|---|---|
| Private bytes (commit, excludes mapped model files) | +128 MiB | `HCS_SOAK_MAX_PRIVATE_GROWTH_MB` |
| Process handles | +32 | `HCS_SOAK_MAX_HANDLE_GROWTH` |
| Threads | +4 | `HCS_SOAK_MAX_THREAD_GROWTH` |
| Decode tokens/s drop, first vs last fifth of the long run | -25 % | `HCS_SOAK_MAX_TPS_DROP_PCT` |

`HttpStorm` samples the **server** process (limits +192 MiB / +48 handles / +8 threads, CLI flags).
The limits are noise bounds, not allowances: a real leak per cycle shows up as a non-zero
`private_slope_mb_per_sample` in `soak-core.jsonl` long before it reaches the limit, and 100 cycles multiply it.
Working set is recorded but not asserted: the model is mmap'd and the OS decides residency.

## Knobs

`HCS_SOAK_MODEL_ROOT` (required), `HCS_SOAK_OUT`, `HCS_SOAK_TEXT_ID`, `HCS_SOAK_IMAGE_ID`, `HCS_SOAK_CYCLES`,
`HCS_SOAK_SWITCH_CYCLES`, `HCS_SOAK_GEN_SECONDS`, `HCS_SOAK_VISION_RUNS`, `HCS_SOAK_IMAGE_RUNS`,
`HCS_SOAK_CANCEL_ROUNDS`, `HCS_SOAK_SHUTDOWN_ROUNDS`, `HCS_SOAK_UNPLUG_ROUNDS`.
Profiles `quick` / `full` in `scripts/soak.ps1` set these; defaults inside the gtest are a seconds-scale smoke.

## Evidence of an actual run

Runs executed so far (CPU build, target machine, models read-only), all `quick` profile; the **full** profile (100
cycles, 30 min generation) has **not** been run:

* `docs/test-evidence/soak/cpu-quick/` (2026-11-18, `902b41e` + the uncommitted W2 port): 12 gtest cases PASS,
  `HttpStorm` FAIL (2 of 18 checks).
* `docs/test-evidence/soak/cpu-quick-w3/` (2026-11-18, `5633450` + the wave-3 working tree, uncommitted): 12 gtest
  cases PASS with the shutdown tests destroying the `Runtime` directly (D-S1 fixed); its `HttpStorm` failed only the
  handle-growth check (`93 > 48`, pool warm-up, see DEFECTS.md D-S2).
* `docs/test-evidence/soak/cpu-quick-w3-httpstorm-rerun/`: `HttpStorm` alone after the harness warms the connection
  pool before its baseline: 18/18 PASS, including C (health under a 64-socket flood) and F (graceful shutdown while
  generating, 0.3 s).
* `docs/test-evidence/soak/w3-ctrl-c/`: manual real-`CTRL_C_EVENT` run (`tests/soak/http_storm/ctrl_c_manual.py`),
  server exits rc=0 in 0.4 s during generation.

Defects and their status: `docs/test-evidence/soak/DEFECTS.md` (D-S1..D-S4 fixed or explained).

`tests/soak/http_storm/http_storm.py` runs `HCSInfer models scan <root>` into its private state directory before
`serve` (on `main`, `serve` only loads the persisted registry). The shutdown tests destroy the `Runtime` directly:
`~Runtime()` joins both job queues first (D-S1), and `RuntimeLifecycle.*` in `tests/contract/api` covers the same
contract with a fake engine, without needing models.

## Defects

Defects found by these tests are recorded in `docs/test-evidence/soak/DEFECTS.md` with a minimal reproducer, the
confirmed cause, the red -> green regression test and the status.
