# ADR-011: Coding agent integration (prime-agent)

Date: 2026-11-17 - Status: proposed (foundations only; no agent runs inside HCSInfer yet) - Owner: AGENT+TRAIN wave

## Context
The V1.1 addendum wants a coding agent inside the HCS desktop app, driven by the local LFM3-Omni-2B model, in the
spirit of Claude desktop. PrimeIntellect-ai/prime-agent (MIT) is the chosen base. It was added as the pinned
submodule `external/prime-agent` at v0.10.4, commit `8980e66f2f07762d62a6c1d73b347a4cac5275cc`, and built from
source on the target machine (evidence: `docs/test-evidence/agent/README.md`).

Facts established by reading the pinned tree and building it (not assumptions):
- It is a **Rust 2021 workspace** of 10 crates (`pa-*`). `cargo build --release` with Rust 1.96.0 on Windows 11
  (MSVC target) succeeded in 10 m 22 s and produced `prime-agent.exe` (65.9 MB) plus a separate `pa-daemon.exe`
  (45.5 MB). `prime-agent --version` prints `0.10.4`.
- It is **not one process**. Sessions live in a background daemon (`pa-daemon`) reached over a local socket; a
  persistent **Python REPL kernel** is the agent's only built-in tool ("everything is programmatic": files, shell,
  sub-agents all go through Python code the model writes). The kernel is bootstrapped through **`uv`** and a venv
  (`prime-agent-runtime`, deps `mcp`, `pyjwt`, `opentelemetry-api`, `tyro`). The Rust code never installs `uv`
  itself (it errors with guidance), but the venv setup pip-installs those packages, which needs network on first run
  (not exercised here; NOT RUN).
- Its README states it is **not a security sandbox**: model-generated Python and commands run with the user's
  permissions.
- **Wire protocol to a local model**: `models.json` in the agent directory (`~/.prime/agent`, overridable with
  `PRIME_AGENT_CODING_AGENT_DIR`) defines custom providers: `{"providers": {"hcs": {"baseUrl": ".../v1", "apiKey": "...",
  "api": "openai-completions", "authHeader": true, "models": [{"id": "...", "contextWindow": N, "maxTokens": N,
  "input": ["text"]}]}}}`. The `openai-completions` client streams `POST /chat/completions` with
  `stream_options: {include_usage: true}`, sends `tools` (function schemas with `strict: false`) and `tool_choice`,
  and reads streamed `tool_calls` deltas. A turn whose tool call has no function name is handled by the client.
  `--offline` / `PI_OFFLINE` disables its startup network operations.
- **Gap in HCSInfer today**: `/v1/chat/completions` answers `501` for `tools`/`functions`/`tool_choice`
  (`docs/api.md`, `src/api/http_api.cpp`). Another wave is adding chat tool support (`tests/unit/api_chat_tools`);
  the agent cannot work against HCSInfer until that is merged and proven with a real model. NOT RUN end to end.

## Decision
1. **Process model: sidecar process tree, not in-process.** HCSInfer.exe starts and supervises the prime-agent
   binary as a child process (and it starts its own daemon). Reasons: it is a separate Rust runtime with its own
   async executor, a Python kernel and a daemon; linking it into the C++ exe is not feasible and would also pull in
   its Rust dependency tree (licences unaudited) into the exe. The agent talks to HCSInfer only through the existing
   loopback HTTP API with the per-run bearer token (ADR-007), exactly like any other client. No private channel.
2. **Single-EXE implication (AGENTS.md 9.5, ADR-008):** the agent breaks the single-file claim. The packaged
   product with the agent is a **portable folder** (`HCSInfer.exe`, `prime-agent.exe`, `pa-daemon.exe`, a bundled
   Python runtime + venv). The single-`HCSInfer.exe` target remains for the runtime (chat/vision/image/API/MCP) and
   the agent is an optional component that the release notes must not describe as part of the single file. An
   alternative to evaluate later: re-implement only the minimal agent loop natively on top of `hcs_agent` (policy)
   and the MCP/HTTP tool surface, and keep prime-agent for developers.
3. **Sandbox and approved roots.** prime-agent's own tools are NOT trusted to enforce policy. HCS policy is
   enforced where HCS owns the boundary:
   - `hcs_agent` (`include/hcs/agent/sandbox_policy.hpp`) is the policy decision layer: argv-only commands (no shell
     string), allow-listed program + pinned sub-command, always-denied arguments, shell/interpreter/network
     programs hard-denied, absolute and `..` paths checked with `PathGuard`, timeout clamp, output cap.
   - Tools exposed to the agent by HCSInfer (an MCP stdio server, ADR-007) call `ToolSandbox` before acting. The
     agent's working directory is a user-chosen approved root, ideally a disposable worktree/clone.
   - The prime-agent Python kernel itself is NOT covered by this policy (it can run arbitrary Python). Until it is
     replaced or sandboxed by the OS, the agent runs only in a user-approved workspace, with a visible warning, and
     "run untrusted code" stays an explicit non-goal. A Windows Job Object / AppContainer / restricted token
     wrapper is the next hardening step and is NOT implemented.
4. **No network by default.** The agent process is started with `--offline`, a provider list that contains only the
   HCS loopback provider, and no `apiKey` other than the HCS token. `SandboxPolicy::allow_network` is `false` and no
   code path sets it. The Python venv bootstrap is the one known network need; it is done once, explicitly, by the
   user ("Prepare agent runtime" action), never silently, and the result is recorded. Verifying that the kernel runs
   with the network blocked is NOT RUN.
5. **Logging:** agent stdout/stderr and tool calls go to HCS structured logs with prompts/file contents redacted by
   default (ADR-007 item 6); only tool name, argv[0], exit code, duration and byte counts are logged. Trajectories
   are written to the user's agent directory and only enter a dataset through the explicit, validated export in
   ADR-012 (never automatically).
6. **Cancel:** the UI Stop action sends cancel through the job interface; the supervisor terminates the child process
   tree (Windows Job Object with kill-on-close) after a grace period. Shutdown of HCSInfer.exe kills the tree.
   Tool commands carry the sandbox timeout (default 60 s, max 600 s).
7. **Pinning and licence.** The submodule is pinned to the commit above and is updated only in a compatibility
   change. MIT; licence text must ship in the portable package. The upstream `install.ps1`/`install.sh` and the
   `irm | iex` / `curl | sh` installers are never used.

## Rejected alternatives
- Linking prime-agent into HCSInfer.exe (Rust+Python+daemon, not feasible, breaks ADR-008 in a worse way).
- Letting the agent execute through its own Python kernel with HCS only "advising" (no enforcement point).
- Running the agent with network access to hosted model providers by default (violates local-first/no-network).
- Writing a new agent from scratch for V1.1 (large; prime-agent already builds and has sessions, subagents, goals).

## Risks / open items
- Chat tool calls in the HCS API are not available yet; model quality for tool calling at 2B parameters is unmeasured.
- Python kernel bootstrap needs `uv` (present on this machine) and network once.
- Agent binary size and the Python runtime add roughly 110 MB plus the venv to the portable package (measured sizes
  of the two executables only).
- Residual risk accepted for allow-listed build tools (cmake, ninja, ctest): they execute project code.

## Evidence
`docs/test-evidence/agent/README.md` (submodule pin, cargo build log summary, unit test results).
