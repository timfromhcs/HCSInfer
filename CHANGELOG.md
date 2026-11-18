# Changelog

Derived from `git log` (all 28 commits are dated 2026-11-17). No release has been tagged; the portable package built
by `scripts/package.ps1` is named `0.1.0`. Entries describe what the commits changed; PASS/NOT RUN state of each
feature is in `docs/roadmap/STATUS.md`.

## Unreleased - V1.1 / V1-stable waves (committed locally on `main`, 3 commits ahead of `origin/main`)

`origin/main` is at `35bd39d` (V1.1 wave 2). The three commits below are committed locally and not yet pushed:
`902b41e` (W1), `5633450` (W2), `b02075f` (W3). Release CTest: **783 registered, 0 failed** (CPU-only build; the
model-dependent tests skip = NOT RUN).

### Added
- MCP tool `hcs.runtime.benchmark` (`suite` quick/full, `model`, `max_tokens`/`warmup`/`iterations`/`seed`/
  `sweep_sizes`): runs the shared text benchmark (the same runner as `HCSInfer benchmark` and
  `POST /hcs/v1/benchmarks`) as a cancellable job and returns the AGENTS.md 21.1 JSON document. Real end-to-end run
  over stdio with LFM3-Omni-2B Q8_0: `PASS`, CPU, cold load 4404 ms, warm TTFT 2574 ms, 18.85 decode t/s, peak RAM
  2.42 GiB - `docs/test-evidence/mcp-benchmark/`. 4 new contract tests; `docs/mcp.md`, README matrix and STATUS
  updated in the same change.
- `POST /hcs/v1/benchmarks` (asynchronous job, `202` + `status_url`, cancellable), `benchmark --suite full`
  (prefill/decode sweeps, `--sweep-sizes`, `SKIPPED`/`FAIL` points, `result_status` `PARTIAL`) and
  `cache stats|clear [--yes] [--json]` (HCS-owned state-dir cache, scratch and orphaned temp files only). Committed in
  W1 (`902b41e`, CPU-build ctest green); a real-model run of the new benchmark paths is NOT RUN. `tune` and
  `models load|unload` stay Planned (exit code 10).
- HCS Agent: process supervisor `hcs_process` (spawn without a shell, bounded output capture, timeout, whole-tree kill
  through a kill-on-close Job Object) (`35bd39d`); prime-agent provider document and launch plan, loopback-only
  (`712b6fa`). A real `prime-agent` session is NOT RUN (ADR-011).
- HCS Train: evaluation-report ingestion, eval gate, adapter book, `HCSInfer train validate|gate|adapter|pipeline`
  (`b4cc9dc`). No trainer is run (ADR-012).
- HCS Studio Web (`web/`): browser-only WebGPU/WASM demo with static security tests and a headless boot check
  (`598e55f`); the WebGPU capability probe times out and falls back to WASM instead of hanging (`8b29907`). Real WebGPU
  inference and the Vercel deploy are NOT RUN (ADR-013).
- V1.1 wave 1 (`6a390b8`): LFM3-Omni-2B Q8_0 as default local LLM with OpenAI `tools` / `tool_calls` over
  `/v1/chat/completions` and a 40-task agentic eval; FLUX.3 Image Klein plumbing (`--llm`, `--vae`, components sidecar,
  `--preset fast|quality`; the render is BLOCKED on gated files); the HCS Studio UI redesign; agent/train foundations.
- V1.1 scope addendum and verified model selection (`e0ff7d4`, `a7527e9`).

- Agent/train port from the stopped `flux3-agent-engine` session, re-based on `main`'s better modules (committed in
  W2, `5633450`):
  `agent_redact`, `event_parser` (hardened: wrongly typed JSON fields no longer throw), `tool_audit` (python tripwire,
  hardened against escapes, f-string expressions, unicode identifiers, aliasing; documented as a tripwire, not a
  sandbox), `session_manager` (consumes `build_launch_plan`, injected launcher, token only via the child environment),
  `chat_template` / `dataset_prep` (bounded against deep JSON, turn-token injection and overwrites) and a rewritten
  `rsi_loop` on the existing `EvalReport`/`AdapterBook`/gate. `hcs::process` gained streaming reads, a replaced
  environment, Job Object process/memory caps and a thread-safe `terminate()`, behind the `IProcessLauncher` seam.
  `ProviderConfig` gained `api_key_env` and `local_server_compat`. Soak harness (`scripts/soak.ps1`, `tests/soak/`,
  `docs/soak.md`) with CTest registration off by default (`-DHCS_SOAK_REGISTER=ON`). Release CTest: 735 registered,
  0 failed (was 550). A real `prime-agent` session, a real trainer run and the Jinja parity check stay NOT RUN.

### Fixed (wave 3, committed `b02075f`; soak defects, see `docs/test-evidence/soak/DEFECTS.md`)
- D-S1: `~Runtime()` now cancels and joins both job queues before any member is destroyed (destroying a `Runtime` with
  a running job used to crash). Red -> green with `RuntimeLifecycle.*`.
- D-S2: `serve` connection budget raised to 48+ workers + 32 queued so ~64 idle / half-open local sockets no longer
  starve `GET /hcs/v1/health` (`serve_transport_options()`).
- D-S3: `serve` shutdown cancels running jobs before stopping the transport (it used to wait for the generation or the
  300 s request timeout) and also handles `SIGBREAK`; the harness's original "Ctrl+C not honoured" was an inherited
  console ignore flag, not the server.
- D-S4 (new): overall request-receive deadline (`TransportOptions::request_deadline_s`, default 15 s) against
  slowloris; needs a one-line access patch of a build-tree copy of `httplib.h` (`cmake/HcsHttplib.cmake`).

### Tests (wave 3, committed `b02075f`; plus the MCP benchmark tool)
- Release CTest 735 -> 779 with wave 3, then **783** with the MCP `hcs.runtime.benchmark` tool, 0 failed
  (model-dependent tests skip = NOT RUN): runtime destruction (4), real-socket
  transport negatives (slowloris, oversized header, malformed request line, bad Content-Length, pipelining, request
  smuggling, long-running request vs deadline, 429 over the wire, health under 64 idle sockets, graceful shutdown: 11),
  storage read-failure/unplug (6), MCP stdio framing (7), log redaction on failure paths and for images (2),
  `atomic_write_file` (6), `json_guard` / `base64` (8). Soak quick re-run: 12 gtest cases PASS (shutdown tests now
  destroy the `Runtime` directly), HttpStorm 18/18 PASS after the harness fixes. Full soak profile NOT RUN.

### Changed
- CI: the `web` job is a blocking gate (`d8a5e37`); format and clang-tidy gates repaired after wave 1 (`06cfbe4`).
  `pr.yml` run 44167798984 is green on all six jobs.

## 0.1.0 - baseline (alpha, not released)

### Added
- Initial HCSInfer core, engines, API, MCP, Model Vault, vision and docs (`d5baf76`): hardware `doctor`, model registry,
  lifecycle, job queue, memory budget arithmetic, path guard, llama.cpp text and libmtmd vision adapter,
  stable-diffusion.cpp adapter, loopback HTTP API, MCP stdio server, HCSMODEL container.
- M3 image routes (`/v1/images/generations`, `/v1/images/edits`) and MCP image tools, M4 embedded web UI, the `benchmark`
  command and the M10 audit fixes D1-D7 (`9cf15e8`).
- Real B0 (CPU) and B1 (Vulkan) text benchmark baselines (`f9cd395`); llama.cpp load-mode matrix and `--load-mode` for
  `benchmark` and `vision` (`0bc77bd`); diffusion compute- vs parameter-backend measurement B7 (`7d263ac`);
  first portable package run (`806c14f`); Vulkan evidence for the image routes and the GUI (`d35685e`).
- Quality gates: clang-format over the whole tree (`b016b50`), first clang-tidy run and reproducible static-analysis
  script (`c486630`), blocking clang-tidy CI job (`5f41c79`, `9f03d74`, `1b45fca`), CI Visual Studio detection via
  vswhere (`fddc30d`), evidence index, token redaction and tracked evidence logs (`bfdaf4d`, `cf7daf2`).

### Known gaps carried forward
- No custom Vulkan kernels, no scheduler/telemetry module, Model Vault not in the engine load path, no single-file EXE,
  `tune`, `cache`, `models load|unload` (CLI), `benchmark --suite full`, `POST /hcs/v1/benchmarks` and Streamable HTTP
  are not implemented.
