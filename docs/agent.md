# HCS Agent: policy layer, sidecar configuration and process supervision (ADR-011)

Status: **partially implemented**. The HCS-owned pieces exist and are tested: the tool-call policy layer, the
prime-agent provider document, the launch plan and a Windows process supervisor (spawn, capture, timeout,
whole-tree kill). **A real `prime-agent` session is NOT RUN** — that needs the Rust build of the sidecar, a
Python runtime bootstrap (`uv`, network) and a model that can tool-call; none of that has been exercised here.

Modules: `hcs_agent` (`include/hcs/agent/`), `hcs_process` (`include/hcs/process/`). Evidence:
`docs/test-evidence/agent/`, pins: `external/dependency-lock.json`.

## The pieces

| Piece | Header | Responsibility |
|---|---|---|
| Policy layer | `hcs/agent/sandbox_policy.hpp` | argv-only command allow-list, pinned sub-command, denied args, hard-denied shells/interpreters/network tools, `PathGuard` for paths and cwd, timeout clamp, UTF-8-safe output cap |
| Provider document | `hcs/agent/agent_config.hpp` | Builds/validates the prime-agent `models.json` provider entry; **loopback-only** and `openai-completions` |
| Launch plan | `hcs/agent/agent_config.hpp` | Builds the sidecar argv (`--offline`, the HCS provider, the model, the token) and env; the workspace must resolve inside an approved root |
| Process supervisor | `hcs/process/child_process.hpp` | Spawns a helper without a shell, captures merged stdout+stderr (bounded), waits with a timeout, and kills the **whole tree** (Windows Job Object, kill-on-close) |

## Why a sidecar, not in-process (ADR-011)

prime-agent is a Rust workspace with its own async runtime, a background daemon over a local socket and a Python
REPL kernel. It is started as a **child process tree** and talks to HCSInfer only through the loopback HTTP API
with the per-run bearer token — the same surface any other client uses. There is no private channel. This also
means it is a **portable-folder** component, not part of the single `HCSInfer.exe` claim (ADR-008); the release
notes must say so.

## Safety invariants enforced in code

* The provider base URL must be `http://127.0.0.1[:port]/v1` (or `localhost` / `[::1]`). Anything else is
  rejected, so a caller cannot point the agent at a remote model (no network by default, ADR-011 item 4).
* The workspace must resolve inside an approved root via `PathGuard`; the launch plan is refused otherwise.
* `--offline` and `PI_OFFLINE=1` are always set; only the HCS provider is present.
* The child is started with an explicit handle list, so it does not inherit HCSInfer's sockets or file handles.
* The child is created suspended, moved into a kill-on-close job object, then resumed — so even a fast child
  cannot escape the job. Killing HCSInfer reaps the tree.
* argv is passed to `CreateProcessW` directly; no shell, so metacharacters are never interpreted.

## C++ API (excerpt)

```cpp
// Build the provider document (loopback-only, validated).
hcs::agent::ProviderConfig provider{...};
auto json = hcs::agent::provider_config_json(provider);   // Result<std::string>

// Build the launch plan (never runs it).
hcs::agent::LaunchRequest request{...};
auto plan = hcs::agent::build_launch_plan(request);       // Result<LaunchPlan>

// Supervise a process: capture output, bound the time, kill the tree.
auto child = hcs::process::ChildProcess::spawn(plan.value().program,
                                               hcs::process::SpawnOptions{plan.value().cwd, plan.value().args,
                                                                          plan.value().env});
auto waited = child.value().wait_for(60000);              // Exited | TimedOut
child.value().terminate();                                // kills the whole tree
```

## Tests

* `tests/unit/agent`: `SandboxPolicy.*`, `AgentConfig.*` (loopback enforcement, provider document shape, launch
  plan scope, workspace escape refused).
* `tests/unit/process`: `ChildProcess.*` against a real helper executable (`tests/unit/process/child_probe.cpp`)
  — merged-output capture, exit code, timeout then terminate, output cap, environment overrides, and a
  **grandchild** that is proven dead after the job object is closed.

## Not done / open

* **No real `prime-agent` session** and no Python venv bootstrap: the supervisor, provider document and launch
  plan are proven against a test helper, not against prime-agent.
* No `AgentSupervisor` composition yet (a thin wrapper that ties a `LaunchPlan` to a `ChildProcess` with
  start/stop/status and structured logging). The building blocks are in place.
* The prime-agent Python kernel is not covered by the policy layer (ADR-011, accepted residual risk); a Windows
  Job Object / restricted-token wrapper for untrusted code is future work.
* No server-side setting/UI to choose the agent workspace yet; the Studio "Agent" tab remains "Planned".
