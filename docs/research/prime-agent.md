# prime-agent as the HCS coding agent: findings

Status: research note, 2026-11-17. Examined: `external/prime-agent`, v0.10.4, commit
`8980e66f2f07762d62a6c1d73b347a4cac5275cc` (MIT). Decision record: `ADR-011`.

## What it is (from README / AGENTS.md / sources)
"A Self-Improving RLM Harness": coding and research agent whose only built-in model tool is a persistent Python
REPL; file, shell and sub-agent work happens through code. Features: sub-agents (`rlm.spawn`), daemon-backed sessions
that survive terminal detach, `/goal`, `/autonomous` with budgets, `/refine` (reviewable harness updates with
rollback snapshots), schedules/heartbeats, TUI, `--mode text|json|rpc|acp|daemon`. Its own README warns that the
worker and kernel processes are not a security sandbox.

## Layout
Rust 2021 workspace, resolver 2, crates `pa-telemetry pa-types pa-ai pa-models pa-agent pa-core pa-daemon pa-tui
pa-cli pa-perf`, a vendored `crossterm`, Python runtime sources in `prime-agent-runtime/` (deps `mcp`, `pyjwt`,
`opentelemetry-api`, `tyro`; bootstrapped through `uv` into a venv). Its AGENTS.md is a contributor guide for that
repo (identity/trailer rules, style, zero-warning lint gates), not instructions for HCSInfer.

## Build (measured)
Toolchain: rustc/cargo 1.96.0, host `x86_64-pc-windows-msvc`. Command (from `external/prime-agent`, own target dir,
no installer script):
`cargo build --release` with `CARGO_TARGET_DIR=build/agent/cargo`. Result: success, 10 m 22 s, 6 compiler warning
lines in the log. Outputs: `prime-agent.exe` 65 864 704 bytes, `pa-daemon.exe` 45 451 264 bytes, `pa-perf.exe`,
`pa-tui-replay.exe`. `prime-agent.exe --version` -> `0.10.4`. Not run: any session, the Python kernel bootstrap, the
test suite (`cargo test`), `cargo deny`.

## Talking to a local OpenAI-compatible endpoint
- Config file: `<agent dir>/models.json` (agent dir `~/.prime/agent` or `PRIME_AGENT_CODING_AGENT_DIR`). Custom
  provider shape (from `crates/pa-core/src/models/custom.rs` and its tests):
  `{"providers":{"hcs":{"baseUrl":"http://127.0.0.1:8765/v1","apiKey":"<HCS token>","api":"openai-completions",
  "authHeader":true,"models":[{"id":"<model id>"}]}}}`; per-model optional `contextWindow`, `maxTokens`, `input`,
  `reasoning`, `compat`, `headers`. `--provider hcs --model <id> --offline` select it on the command line.
- Request format (from `crates/pa-ai/src/providers/openai_completions`): `POST {baseUrl}/chat/completions`,
  `stream: true`, `stream_options.include_usage: true`, `tools` as `{"type":"function","function":{name,description,
  parameters,strict:false}}`, optional `tool_choice`, `max_tokens`/`max_completion_tokens` per `compat.maxTokensField`;
  the response must stream `delta.tool_calls[]` (index, id, function.name, function.arguments fragments),
  `finish_reason: "tool_calls"`, a final usage chunk, and `data: [DONE]`.
- HCSInfer today: `/v1/chat/completions` returns 501 for `tools`/`tool_choice`, so this integration is blocked until
  chat tool calling lands in the API and is proven with a real model (a separate wave owns it). It must also accept
  `stream_options` (already tolerated) and emit the streamed `tool_calls` shape above.

## Constraints that shape the product
- Not single-EXE: agent exe + daemon + Python runtime/venv (ADR-011 decision 2).
- First-run needs `uv` and network for the venv (uv is installed on this machine; the Rust code does not install it).
- Executes model-written Python with user permissions: HCS policy (`hcs_agent`) must sit at every HCS-provided tool;
  the kernel itself needs OS-level confinement before the agent is offered for untrusted repositories.
- Quality ceiling: a 2B local model driving an RLM-style Python harness is untested; the eval gate in ADR-012 is how
  improvement is measured.
