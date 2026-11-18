# HCS Agent: policy layer, tool audit, session manager and process supervision (ADR-011)

Status: **implemented and unit-tested against a stand-in agent; a real `prime-agent` session is NOT RUN.** The
HCS-owned pieces exist: tool-call sandbox policy, the prime-agent provider document and launch plan, secret
redaction, the `--mode json` event parser, the python tool-call audit (a tripwire, see below), the session manager
and the single Windows Job-Object process launcher. What has not been exercised is the Rust build of
`prime-agent`, its Python kernel bootstrap (`uv`, network) and a model that can tool-call reliably — none of that
is available to the tests. They run the real launcher against `hcs_child_probe`, a helper executable that prints
the same JSON-lines events `prime-agent --mode json` does.

Modules: `hcs_agent` (`include/hcs/agent/`), `hcs_process` (`include/hcs/process/`). Pins:
`external/dependency-lock.json`. Evidence: `docs/test-evidence/agent/`.

## The pieces

| Piece | Header | Responsibility |
|---|---|---|
| Policy layer | `hcs/agent/sandbox_policy.hpp` | argv-only command allow-list, pinned sub-command, denied args, hard-denied shells/interpreters/network tools, `PathGuard` for paths and cwd, timeout clamp, UTF-8-safe output cap |
| Provider document + launch plan | `hcs/agent/agent_config.hpp` | Builds/validates the prime-agent `models.json` entry (loopback-only, `openai-completions`) and the sidecar argv/env; the workspace must resolve inside an approved root. `api_key_env` keeps the token out of argv and out of `models.json` |
| Secret redaction | `hcs/agent/agent_redact.hpp` | `redact_secrets` (the session token and common credential shapes) and `cap_utf8` (cut on a code-point boundary) |
| Event parser | `hcs/agent/event_parser.hpp` | One line of `--mode json` output to HCS events; wrongly typed fields, deep nesting and binary junk produce no event and never throw |
| Tool audit | `hcs/agent/tool_audit.hpp` | Static scan of each `python_repl` call (see "Tripwire, not a sandbox") |
| Session manager | `hcs/agent/session_manager.hpp` | Start, supervise, stream and cancel one agent session at a time; consumes `build_launch_plan`; injected launcher |
| Process supervisor | `hcs/process/child_process.hpp` | Spawn without a shell; capture merged output **or** stream separate stdout/stderr; replaced environment; Job Object with kill-on-close, active-process cap, memory cap and UI restrictions |
| Launcher seam | `hcs/process/launcher.hpp` | `IProcessLauncher` / `IChildProcess` over `ChildProcess`; the only launcher (tests substitute a scripted fake) |

## Why a sidecar, not in-process (ADR-011)

prime-agent is a Rust workspace with its own async runtime, a background daemon over a local socket and a Python
REPL kernel. It is started as a **child process tree** and talks to HCSInfer only through the loopback HTTP API
with a per-run bearer token — the same surface any other client uses. There is no private channel. It is a
**portable-folder** component, not part of the single `HCSInfer.exe` claim (ADR-008); the release notes must say so.

## Tripwire, not a sandbox

`ToolAudit` is a **tripwire**. prime-agent's only model tool is `python_repl`: the model writes Python and a kernel
runs it with the user's permissions. HCS cannot put a policy check between the model and that kernel, so every
`tool_execution_start` event is inspected the moment it is seen and, on a violation, the session manager kills the
whole process tree (`PolicyKilled`). The check is a token-level scan (a small Python lexer: strings and comments are
lexed, so a docstring that mentions `exec(` does not trip it) that

* refuses any tool other than `python_repl`;
* refuses dynamic code and interpreter-internals escapes: `exec`, `eval`, `compile`, `breakpoint`, `__import__`,
  `importlib`, `ctypes`, `runpy`, `pickle`, `__builtins__`, `__subclasses__`, `__globals__` ..., also when `exec`/`eval`
  is only referenced and not called, and any non-ASCII identifier (Python NFKC-normalises identifiers, so a
  full-width `ｅｘｅｃ` is `exec`);
* refuses network and process modules in every import form (`import a, socket`, `from urllib.request import ...`,
  `import x; import socket`, ...) and aliasing of `os` / `subprocess` that would hide process calls;
* decodes `\xNN`, `\uNNNN`, `\UNNNNNNNN` and octal escapes before judging a path or command, follows implicit string
  concatenation and backslash line continuations, and audits the `{expression}` parts of f-strings (nested up to
  four levels) as code;
* extracts every **literal** shell command (`bash('...')`, `os.system`, `subprocess.run([...])`, ...) and checks it
  against `sandbox_policy` (allow-listed programs only, no shell metacharacters, no URLs, paths inside the root); a
  non-literal command is refused as unauditable;
* refuses absolute paths, `..` segments and `~` in string literals that resolve outside the approved root.

### What it does NOT do

A lexical scan cannot follow data flow. These are **not** covered, and the tests list them as documented limits
rather than coverage (`DocumentedLimit_*` in `tests/unit/agent/test_tool_audit.cpp`):

* paths or commands assembled at run time (`chr(47) + 'etc' + ...`, `os.listdir` + `os.remove`, `shutil.rmtree`);
* an allow-listed build tool that runs project code (`python -m unittest`, `make`, `cmake`) can do anything the
  project's code does;
* a race between the audit event and the tool actually starting, and anything the audit's heuristics call harmless.

The actual containment is the Job Object (kill-on-close, process count, committed-memory cap, no UI/clipboard), the
replaced child environment, the per-session TEMP/HOME, the approved root and **a disposable workspace**. Run
untrusted repositories in an external sandbox (ADR-011, "Residual risk"). Do not describe `ToolAudit` as isolation.

## Session manager

One session = one `prime-agent --mode json -p` child. `SessionManager::create` validates everything synchronously
(task size, root inside an approved root and not containing the agent state directory, executable and kernel Python
present, loopback provider, model id), builds the command line through `build_launch_plan`, writes `models.json`
and starts a runner thread.

* **Token handling.** The token is passed only through the child's environment (`HCS_AGENT_TOKEN`, via
  `ProviderConfig::api_key_env`). `models.json` contains the variable name, argv never contains the token, and
  every event, log line, persisted `events.jsonl` and the task preview are passed through `redact_secrets`.
* **Environment.** Rebuilt from an allow-list (`SystemRoot`, `PATH`, ...), a private `HOME`/`APPDATA`/`TEMP` per
  session, telemetry off. Nothing else of the parent's environment reaches the child (`inherit_environment = false`).
* **Bounds.** Wall-clock timeout (watchdog), one concurrent session (`Busy` otherwise), `max_events`,
  `max_total_event_bytes`, `max_line_bytes` (an oversized line is dropped and the stream recovers), at most 200
  stderr log events, job caps from `SessionLimits`. The final `session_end` event is never dropped.
* **Cancel / shutdown.** `cancel` and `shutdown` terminate the whole tree; the destructor joins every runner.
* **Outcome.** `Completed` needs exit code 0 **and** an `agent_end` event; a policy violation is `PolicyKilled`;
  cancel, timeout and launch failure have their own states.
* **Events.** `events(id, after, wait_ms, max_count)` is a bounded long poll; ids are validated (`ag-` + 16 hex).

## Process supervisor changes for the agent

`hcs::process::SpawnOptions` gained (defaults keep the old behaviour):

| Field | Meaning |
|---|---|
| `inherit_environment` | `false`: `env` is the complete child environment |
| `max_active_processes`, `max_job_memory_bytes` | Job Object `ACTIVE_PROCESS` / `JOB_MEMORY` limits for the whole tree |
| `restrict_ui` | no desktop, clipboard, global atoms, system parameters |
| `stream_output` | separate stdout/stderr pipes read with `read_stdout` / `read_stderr`; stdin is `NUL`; nothing is buffered internally, so stderr can never interleave into a stdout line (an interleaved line could hide a tool-call event from the audit) |

`terminate()` is idempotent and safe from any thread, also while another thread is blocked in `wait_for()` or a
streaming read; a killed child reports its real exit code.

## C++ API (excerpt)

```cpp
// Provider document + launch plan: the token travels via the child environment only.
hcs::agent::ProviderConfig provider{...};
provider.api_key_env = "HCS_AGENT_TOKEN";
auto doc  = hcs::agent::provider_config_json(provider);   // apiKey = "HCS_AGENT_TOKEN" (a name, not a secret)
auto plan = hcs::agent::build_launch_plan(request);       // args without --api-key, env["HCS_AGENT_TOKEN"] = token

// Sessions.
hcs::agent::SessionManager manager(settings, approved_roots, hcs::process::make_native_launcher(), logger);
auto id   = manager.create({workspace, "make the tests pass", std::nullopt});
auto page = manager.events(id.value(), /*after=*/0, /*wait_ms=*/5000);
manager.cancel(id.value());
```

## Tests

* `tests/unit/agent`: `SandboxPolicy.*`, `AgentConfig.*` (incl. token-via-env), `AgentRedact.*` (secret shapes,
  UTF-8 boundary cap), `EventParser.*` (wrong types, 200 000-deep nesting, 5 MiB lines, mutation fuzz — never
  throws), `AuditFixture.*` (adversarial: dynamic code, unicode look-alikes, f-string expressions, escapes,
  continuations, aliasing, network/process modules, literal shell commands vs `sandbox_policy`, documented limits),
  `Session*` (fake launcher: validation, argv/env/limits, redaction, event and byte caps, policy kill, timeout,
  cancel, concurrency, eviction, shutdown races; real launcher + `hcs_child_probe`: Job Object kill of a
  grandchild, replaced environment, token never in argv or foreign variables, flood and oversized-line caps).
* `tests/unit/process`: `ChildProcess.*`, `ChildProcessStream.*`, `ChildProcessEnv.*`, `ChildProcessJob.*` (a fork
  bomb is stopped by the active-process cap, a runaway allocation by the memory cap) and `NativeLauncher.*`, all
  against a real helper executable.

## Not done / open

* **No real `prime-agent` session** and no Python venv bootstrap: everything above is proven against the stand-in.
* The prime-agent Python kernel is not covered by the policy layer beyond the tripwire (accepted residual risk).
* The session manager is not yet reachable from the HTTP API, MCP or the Studio "Agent" tab (it stays "Planned").
* `ToolAudit` is Windows-agnostic logic; the launcher is implemented for Windows only (other platforms return
  `Unsupported`, NOT RUN).
