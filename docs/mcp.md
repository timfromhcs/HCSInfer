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
| `hcs.image.generate` | `prompt`, `model?`, `negative_prompt?`, `width?`, `height?` (64..2048, multiples of 8, <= 1 Mpixel), `steps?` (1..150), `seed?`, `cfg_scale?`, `loras?` (max 4 `{id, weight?}`), `upscale?` (`off`\|`4k`; same fields on `hcs.image.edit`) | text-to-image with the loaded (or given) image model. Returns an MCP **image content block** (`type:"image"`, base64 PNG, `mimeType:"image/png"`) plus a text block and `structuredContent` = `{job_id, model, format, width, height, steps, generation_ms, bytes}` (no pixels). Cancellation (`notifications/cancelled`) and the call timeout stop the real render. No image engine linked -> `unsupported`; a text model named -> `invalid_argument`; no image model loaded -> `invalid_state` |
| `hcs.loras.list` | (none) | the image LoRA library (read-only; ids, base families, trigger words, weights, licence, status; never paths). Ids are what `loras` on the image tools accepts |
| `hcs.image.edit` | `prompt`, `input_path`, `model?`, `negative_prompt?`, `width?`, `height?`, `steps?`, `strength?` (0..1), `seed?`, `cfg_scale?` | image-to-image of a png/jpeg file. `input_path` goes through `PathGuard` **first** (outside the approved roots or with `..` -> `security_violation`; nothing is read), is size-capped and header-probed (non-image -> `invalid_argument`). Output size defaults to the input's. Result as for `hcs.image.generate` |
| `hcs.runtime.status` | `include_devices?` | engine, loaded model, memory budget, optional device report |
| `hcs.runtime.benchmark` | `model`, `suite?` (`quick`\|`full`), `max_tokens?` (<=4096), `warmup?` (0..20), `iterations?` (1..50), `seed?`, `sweep_sizes?` (16..1048576, only with `full`) | runs the shared text benchmark (the same runner as `HCSInfer benchmark` and `POST /hcs/v1/benchmarks`) as a cancellable job; returns the AGENTS.md 21.1 JSON document. The model is loaded/unloaded through the normal lifecycle and left loaded. No text engine -> `unsupported`; an image-model id or bad `sweep_sizes` -> `invalid_argument` |
| `hcs.runtime.shed` | none | kill switch (destructive): cancels all jobs and unloads the model, bounded by the shed deadline; returns the shed report (see [overload-protection.md](overload-protection.md)) |
| `hcs.jobs.status` / `hcs.jobs.cancel` | `job_id` (`job-N`) | |

Security: there is no shell, process, or arbitrary filesystem tool. File arguments (`image_path`, `input_path`) are
resolved through `PathGuard` (approved roots = `--model-root` + `settings.json`; separate input/output roots are future
work). Models are loaded only by registered id.

## Example (modern era)

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"hcs.chat.generate","arguments":{"prompt":"Say hi","max_tokens":32},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}}}}
```

Client configuration (generic stdio command): `HCSInfer.exe mcp stdio --model-root E:\Models`.
