# Architecture (current implementation + target)

Target architecture: AGENTS.md section 3. This document records what exists now (M2 scaffold) and the rules
other modules must follow.

## Layers

```
HCSInfer.exe (src/app)            -> hcs::cli::run(args, out, err)
  src/cli  (hcs_cli)              command router, --help at every level, stable exit codes
    |  optional: hcs_hardware (doctor)   [later: hcs_api, hcs_mcp, hcs_gui]
  src/core (hcs_core)             control-plane primitives (below)
  include/hcs/engine.hpp          ITextEngine / IImageEngine (opaque value types)
  engine adapters (planned)       src/engines/llama, src/engines/diffusion -> only place upstream types may appear
```

## hcs_core contents

| Header | Responsibility |
|---|---|
| `result.hpp` | `Result<T>`, `Error`, `ErrorCode` with stable exit codes (0,2-14,70) |
| `checked_math.hpp` | overflow-safe u64 add/mul/sub, `range_within` |
| `lifecycle.hpp` | `ModelLifecycle` state machine (AGENTS 10.3), mutex-serialised, compare-and-transition |
| `model_registry.hpp` | manifests, id rules, read-only scan with header validation, JSON persistence |
| `job_queue.hpp` | bounded queue, worker threads (default 1), cancellation tokens, per-job event stream |
| `memory_budget.hpp` | budget from supplied measurements, single-entry-per-region ledger (no double counting) |
| `settings.hpp` | versioned settings, v0->v1 migration, atomic write, loopback-by-default validation |
| `path_guard.hpp` | approved-root canonicalisation; rejects `..`, NUL, device paths, ADS, link escapes |
| `fs_util.hpp` | atomic file replace, bounded file read |
| `logger.hpp` | JSON-lines logger on stderr (never prompts/images) |
| `engine.hpp` | engine interfaces + request/metrics value types |

## Engine adapters

### Llama text adapter (`src/engines/llama`, target `hcs_engine_llama`)

Public header: `include/hcs/engines/llama_engine.hpp` (class `hcs::engines::LlamaTextEngine : ITextEngine`). No
`llama.h`/`ggml.h` type leaves `src/engines/llama/llama_engine.cpp`.

- **Upstream integration**: `external/llama.cpp` (pin d3c3e242c16b8cbf51400fad410cd69b22d64f35) via
  `add_subdirectory`, static libs, common/tests/examples/tools/server/openssl OFF, `GGML_VULKAN` follows
  `HCS_ENABLE_VULKAN`. Same MSVC runtime (`/MD`) as all HCS targets. `HCS_LLAMA_VULKAN` is defined for the adapter
  when Vulkan is compiled in. A second ggml consumer (stable-diffusion.cpp) in the same graph is the open ADR-001
  question; this module guards its `add_subdirectory` with `if(NOT TARGET llama)`.
- **API generation at the pin**: `llama_decode` is deprecated; the adapter uses `llama_batch_ext_*` +
  `llama_process(LLAMA_PROCESS_TYPE_DECODE)`; the load mode uses `llama_model_params::load_mode`.
- **Backends**: `Backend::Cpu` => empty device list, `n_gpu_layers=0`, KV on host. `Backend::Vulkan` => needs a ggml GPU
  device (else `Unsupported` for builds without Vulkan, `BackendError` without a device); no silent fallback.
  `Backend::Auto` => GPU if one exists, else CPU. Backend identity (device description, `offloaded N/M layers`)
  is parsed from the captured upstream log and exposed through `EngineMetrics` / `last_load_log()`.
- **Lifecycle / rollback**: all upstream objects live in one `Session` (context freed before model). A failed load
  leaves the engine unloaded and `load()` repeatable; upstream log text is mapped to `OutOfMemory` / `Unsupported` /
  `ModelError` / `BackendError`. Loading over a loaded model is refused (`InvalidState`).
- **Jobs**: `generate()` submits a `JobQueue` job; output is streamed as `JobEventKind::Output` events (UTF-8
  complete chunks only). Jobs hold only a `weak_ptr<Session>`, so `unload()` can cancel all of this engine's jobs
  (queued ones become `Cancelled`), wait for them, then drop the last strong reference which frees the context and
  model before `unload()` returns. Context ownership is serialised by a per-session mutex.
- **Cancellation**: checked between decode steps and wired to `llama_set_abort_callback` (aborts long CPU prefill).
- **Prompting**: the model's embedded chat template (`llama_model_chat_template` + `llama_chat_apply_template`) is
  applied to `system_prompt` + `prompt`; `apply_chat_template=false` sends the prompt verbatim. Each request is
  single-turn (KV cache cleared). Prompts are never logged.
- **Metrics** (`EngineMetrics`): `load_ms`, `ttft_ms` (job start to first token, excludes queue wait),
  `prompt_tokens(_per_second)`, `decode_tokens(_per_second)` (tokens after the first), `resident_bytes`
  (model size), `offloaded_layers/total_layers`.
- **Vision (libmtmd)**: `LoadRequest::projector_path` (the mmproj GGUF) makes `load()` also create an
  `mtmd_context` (same device as the text model, freed before the model/context in `Session`). `analyze_image()` is a
  normal cancellable job: header probe (`mtmd_helper_get_media_meta`, 16 Mpixel / 32 MiB caps before any decode) ->
  decode (stb, png/jpeg/bmp/gif; video/audio rejected) -> chat template with the media marker in the user turn ->
  `mtmd_tokenize` -> `mtmd_helper_eval_chunks` (text decode + image encode/decode) -> the same sampler/decode loop as
  text. Missing/mismatching projector, projector-as-main-model and corrupt images are clean `Unsupported` /
  `ModelError` / `InvalidArgument` errors. The API layer reaches it through `hcs::api::IVisionStreamEngine`
  (`include/hcs/api/vision_stream.hpp`); `ControlPlane` pairs a model with the `mmproj*.gguf` in its folder
  (`projector_path_for`) and routes chat messages that carry an image (`submit_vision_chat`). The libmtmd build
  renames its private stb copy (`hcs_mtmd_*`) so it can coexist with stable-diffusion.cpp's in one process.
- **Not implemented here yet**: memory-budget accounting of the projector, multi-image / multi-turn image chat,
  video/audio input, KV-cache type options, sampler options beyond temperature/seed.
- **CLI**: `HCSInfer chat --model <path-or-id> [--prompt ...] [--backend cpu|vulkan] [--json]` (see `chat --help`);
  `HCSInfer vision --model <path-or-id> --image <file> --prompt ... [--mmproj <file>] [--json]` (see `vision --help`).
  Ctrl-C cancels the running job (exit 6).
- **Env**: `HCS_LLAMA_VERBOSE=1` echoes upstream log lines to stderr.
- **Tests**: `tests/integration/llama_engine` (label `integration`). Error/validation tests always run; real-model
  tests need `HCS_TEST_GGUF` and otherwise report `NOT RUN` (skipped with that message).

## Rules

- GUI, CLI, API and MCP call control-plane services only; `llama_*`/`sd_*` appear only in adapters.
- Long operations are jobs (`JobQueue`) with cancellation and structured errors; model state changes go through
  `ModelLifecycle` (exactly one winner of a concurrent load).
- The memory budget is pure arithmetic over measurements supplied by `hcs_hardware`; shared (UMA) allocations are
  one ledger entry keyed by region id.
- Unknown hardware values are `"unknown"`, never guessed.
- stdout is reserved for command output / MCP JSON-RPC; logs go to stderr.

## Decisions

ADR-002 (process model), ADR-003 (Vulkan ownership), ADR-004 (memory budget): `docs/decisions/`.
Remaining ADRs from AGENTS.md section 4 (001, 005-010) are listed in the implementation plan.
