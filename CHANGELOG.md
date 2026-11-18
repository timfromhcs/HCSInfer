# Changelog

Derived from `git log` (all 28 commits are dated 2026-11-17). No release has been tagged; the portable package built
by `scripts/package.ps1` is named `0.1.0`. Entries describe what the commits changed; PASS/NOT RUN state of each
feature is in `docs/roadmap/STATUS.md`.

## Unreleased - V1.1 (work in progress on `main`, `35bd39d`)

### Added
- `POST /hcs/v1/benchmarks` (asynchronous job, `202` + `status_url`, cancellable), `benchmark --suite full`
  (prefill/decode sweeps, `--sweep-sizes`, `SKIPPED`/`FAIL` points, `result_status` `PARTIAL`) and
  `cache stats|clear [--yes] [--json]` (HCS-owned state-dir cache, scratch and orphaned temp files only). Uncommitted
  at the time of writing; CPU-build ctest 550 registered, 0 failed; a real-model run of the new benchmark paths is
  NOT RUN. `tune` and `models load|unload` stay Planned (exit code 10).
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
