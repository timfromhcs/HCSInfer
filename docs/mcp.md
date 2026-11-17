# HCSInfer MCP server (stdio)

Status: **Experimental (M3)**. Implemented in `src/mcp` (`McpServer`), started with:

```powershell
HCSInfer.exe mcp stdio --model-root E:\Models
```

Streamable HTTP is **not implemented** and is not claimed. Only the stdio transport exists.

## Protocol version (verified against the spec site)

On 2026-11-17 https://modelcontextprotocol.io/specification/versioning states: "The current protocol version is
**2026-07-28**". Relevant facts checked in the 2026-07-28 pages (versioning/compatibility, stdio transport,
cancellation, tools, key-changes):

* The `initialize` handshake is **removed**; every request carries `_meta."io.modelcontextprotocol/protocolVersion"` and
  `_meta."io.modelcontextprotocol/clientCapabilities"`; `server/discover` is mandatory; results carry `resultType`;
  `ping` is removed; `tools/list` results carry `ttlMs` + `cacheScope`.
* Version mismatch -> JSON-RPC error `-32022` with `data: {supported: [...], requested: "..."}`.
* stdio: newline-delimited messages, no embedded newlines; server stdout carries only MCP messages; logs go to stderr;
  cancellation is the `notifications/cancelled` notification and a cancelled request gets **no** response; closing stdin
  is the shutdown signal.

HCSInfer is **dual-era**:

| Era | Versions | How it is selected |
|---|---|---|
| modern (stateless) | `2026-07-28` | every request carries `_meta.io.modelcontextprotocol/protocolVersion` = `2026-07-28` and `clientCapabilities` |
| legacy (handshake) | `2025-11-25` | the client sends `initialize` (then `notifications/initialized`); this selects legacy semantics for the process lifetime; `ping` works only here |

Behaviour details:

* `server/discover` returns `{supportedVersions: ["2026-07-28","2025-11-25"], capabilities: {tools: {listChanged:false}},
  serverInfo, instructions, resultType:"complete"}`. **Caveat:** the exact `DiscoverResult` field names were confirmed
  only for `supportedVersions` (stdio compatibility page); the remaining fields follow the common
  capabilities/serverInfo/instructions shape and were not verified against the schema page.
* A request without the `_meta` version -> `-32602` (with a hint to send `initialize` for legacy clients); unknown
  version -> `-32022`; `2025-11-25` in `_meta` without a handshake -> `-32602`.
* `initialize` always answers `protocolVersion: "2025-11-25"`; the client decides whether to continue.
* Not implemented: resources, prompts, logging, subscriptions/`subscriptions/listen`, tasks extension, multi
  round-trip `input_required` results, progress notifications, JSON-RPC batches (answered with `-32600`).

## Transport rules implemented

* stdout carries **only** JSON-RPC messages (one per line, UTF-8, invalid UTF-8 replaced); the structured logger writes
  JSON lines to **stderr**. Verified by `McpStdio.StdoutCarriesOnlyJsonRpc...` and the real-binary e2e.
* Input lines are bounded (1 MiB); an over-long line is discarded and answered with `-32600`, the stream resynchronises
  at the next newline.
* `tools/call` runs on worker threads (default max 4 concurrent, then a `busy` tool error) so the reader keeps
  processing `notifications/cancelled`. Cancellation cancels the underlying job (the engine stops) and **suppresses the
  response**. Unknown/finished ids are ignored.
* Hard per-call timeout (`--timeout-s`, default 300 s) cancels the job and returns a `timeout` tool error.
* EOF on stdin: in-flight calls get up to 10 s to finish, then are cancelled; the process exits `0`.

## Tools

All input schemas are closed (`additionalProperties:false`), enforced server-side by the same validator that advertises
them (`src/api/json_schema.cpp`). Invalid arguments are returned as tool errors (`isError:true`, code
`invalid_argument`) per the spec; an unknown tool name is `-32602`. Every result has a text `content` block (the JSON)
and `structuredContent`. Tool errors have `structuredContent.error = {code, message}` using the stable HCS error names.

| Tool | Arguments | Notes |
|---|---|---|
| `hcs.models.list` | - | registered models + state (no file paths) |
| `hcs.models.inspect` | `id` | |
| `hcs.models.load` | `id` | cancellable job; unloads the previous model; file must be in an approved root |
| `hcs.models.unload` | - | cancels running generation first |
| `hcs.chat.generate` | one of `prompt` / `messages`; `model`, `max_tokens` (<=4096), `temperature`, `seed` | returns `{text, finish_reason, usage, job_id, model}` |
| `hcs.vision.analyze` | `prompt`, `image_path`, `model?`, `max_tokens?`, `temperature?`, `seed?` | answers a question about a png/jpeg/gif/bmp file with the loaded (or given) multimodal model; returns `{text, finish_reason, usage, job_id, model}`. The path is resolved through `PathGuard` first (outside the approved roots -> `security_violation`), size-capped, header-probed (non-image -> `invalid_argument`); a model without its `mmproj*.gguf` projector -> `unsupported` |
| `hcs.image.generate` | `prompt`, `width`, `height`, `steps`, `seed`, `model?` | **unsupported** (no image engine) |
| `hcs.image.edit` | `prompt`, `input_path`, `model?` | **unsupported**; path guarded first |
| `hcs.runtime.status` | `include_devices?` | engine, loaded model, memory budget, optional device report |
| `hcs.jobs.status` / `hcs.jobs.cancel` | `job_id` (`job-N`) | |

Security: there is no shell, process, or arbitrary filesystem tool. File arguments (`image_path`, `input_path`) are
resolved through `PathGuard` (approved roots = `--model-root` + `settings.json`; separate input/output roots are future
work). Models are loaded only by registered id.

## Example (modern era)

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"hcs.chat.generate","arguments":{"prompt":"Say hi","max_tokens":32},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
```

Client configuration (generic stdio command): `HCSInfer.exe mcp stdio --model-root E:\Models`.
