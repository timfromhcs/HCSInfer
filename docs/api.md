# HCSInfer HTTP API

Status: **Experimental (M3)**. Implemented in `src/api` (`ApiHandler` = all rules, `HttpServer` = socket
transport on cpp-httplib v0.44.0). Inference requires an engine adapter that implements
`hcs::api::ITextStreamEngine`; the llama.cpp adapter is wired in when the binary is built with
`external/llama.cpp`. Image generation/editing (`/v1/images/*`) needs an engine that implements
`hcs::api::IImageStreamEngine`; the stable-diffusion.cpp adapter is wired in when the binary is built with
`external/stable-diffusion.cpp` (otherwise those routes answer `501 unsupported` with the reason). The embedded web UI
(`GET /`, `/ui/*`, see `docs/gui.md`) is served by the same listener.

## Starting the server

```powershell
HCSInfer.exe serve --model-root E:\Models            # 127.0.0.1:8765, random token printed on stderr
HCSInfer.exe serve --port 0 --model-root E:\Models   # free port; the bound port is printed on stderr
```

stderr shows `HCSInfer API listening on http://127.0.0.1:<port>` and
`HCSInfer API token: <64 hex chars>`. The token is random per run; pass `--token <secret>` (min 16 chars) or set
`HCS_API_TOKEN` to choose your own (then it is not printed).

| Option | Meaning |
|---|---|
| `--host <addr>` | bind address, default `127.0.0.1`. A non-loopback address is refused unless `--lan` is given |
| `--port <n>` | default 8765, `0` = pick a free port |
| `--lan` | explicit opt-in for network exposure: binds `0.0.0.0` (unless `--host`), accepts IP-literal `Host` headers. Auth is always on |
| `--token`, `HCS_API_TOKEN` | fixed bearer token |
| `--allow-host <name>` | extra accepted `Host` name (repeatable) |
| `--allow-origin <origin>` | extra accepted browser `Origin` (repeatable) |
| `--model-root <dir>` | approved model directory (repeatable; adds to `model_roots` in `settings.json`) |
| `--max-body-bytes <n>` | request body cap, default 1 MiB |
| `--max-inflight <n>` | concurrent request cap, default 8 |
| `--timeout-s <n>` | per-request generation timeout, default 300 |
| `--min-free-ram-mb <n>` / `--hard-floor-mb <n>` / `--max-commit-pct <n>` / `--max-active-jobs <n>` / `--watchdog-ms <n>` / `--no-overload-protection` | overload protection thresholds, see [overload-protection.md](overload-protection.md) (defaults: keep >= 2560 MiB free, kill switch below 1024 MiB, commit < 90 %, 4 jobs in flight) |
| `--chat-template auto\|chatml\|plain` | see "Chat prompt construction" |
| `--vault-prefetch` | opt-in Model Vault load stage (default **off**): read each model file once through the bounded async reader and check its SHA-256 before the engine opens it. See `docs/model-vault.md` for what was measured |
| `--vault-buffers <n>`, `--vault-chunk-mib <n>` | read-ahead slots (1..16, default 4) and read size (1..128 MiB, default 16) of that stage |

Models are those registered by `HCSInfer models scan <dir>` (registry in the state dir). The server never scans or
writes model files; a model only loads if its registered path resolves **inside an approved root**.

## Controlling a running server from the CLI

A model lives in the process that loaded it, so the one-shot CLI cannot hold one. `models load|unload|status` are
therefore plain HTTP clients of a running `serve` (they call the routes in "HCS-native routes"):

```powershell
$env:HCS_API_TOKEN = "<token serve printed>"           # or pass --token <secret>
HCSInfer.exe models status                              # health, loaded model, pending jobs   (token optional: health only)
HCSInfer.exe models load qwen-q4                        # POST /hcs/v1/models/qwen-q4/load, waits for the job
HCSInfer.exe models unload                              # unloads whatever is loaded (or: unload <id>)
```

Connection options: `--host` (default 127.0.0.1), `--port` (default 8765), `--token`; load also takes `--timeout-s`
(default 900; the server-side job keeps running when the CLI gives up) and `--no-wait`; all take `--json`.
Exit codes are the API's own error codes: `0` ok, `2` bad argument, `3` model error, `5` out of memory, `6` cancelled,
`7` unknown model, `8` **no server reachable** (the message says so and names `HCSInfer serve`), `9` bad or missing
token, `11` nothing loaded / invalid state, `14` timeout. Covered by `tests/contract/cli/test_cli_remote.cpp` (real
sockets, real bearer auth; the engine behind that server is a fake, so it proves the client and its exit codes, not
inference) and by `tests/e2e/api_mcp_real` (real exe + real model).

`HCSInfer models hash <id>` records the SHA-256 of a registered model file in the registry (read-only pass, nothing is
written next to the model); `serve --vault-prefetch` then refuses to load a file whose digest differs.

## Security model

(Decision record: [ADR-007](decisions/ADR-007-api-mcp-security.md).)

* **Loopback by default.** `--lan` is the only way to bind elsewhere; the token is mandatory in every mode.
* **Bearer token** on every route except `GET /hcs/v1/health`, which returns only `{"status","version"}`. Constant-time
  comparison; an empty configured token never authenticates.
* **DNS-rebinding defence:** the `Host` header must be `localhost`, `127.0.0.1`, `[::1]` (any port), the bind host, an
  `--allow-host` name, or - only with `--lan` - an IP literal. Otherwise `403 host_not_allowed`, even with a valid token.
* **Origin validation:** if an `Origin` header is present it must be same-origin (loopback host **and this server's
  port**) or in `--allow-origin`; otherwise `403 origin_not_allowed`. `null` origins are rejected. **No CORS headers are
  ever emitted**, so browsers cannot read responses cross-origin.
* **Limits:** body cap (transport answers `413` before reading the body), JSON nesting depth 64, schema validation,
  `max_tokens` cap 4096, prompt cap 256 KiB, 128 messages, concurrency cap (`429` + `Retry-After`), request timeout
  (`504`, the job is cancelled). Health and job cancellation are never throttled. Transport: connections that never send a
  request are dropped after 2 s, a request must be received completely within 15 s (a client dripping header bytes is
  disconnected), the worker pool (48+ threads) is sized well above `--max-inflight` so ~64 idle local sockets cannot
  starve `GET /hcs/v1/health`, and connections beyond the pool plus a 32-entry queue are closed at accept time. A
  `SIGINT`/`SIGTERM`/`SIGBREAK` cancels running jobs first, then stops the transport.
* **No path input.** Models are addressed by id (`^[a-z0-9][a-z0-9._-]{0,63}$`); file paths come only from the
  registry and are re-checked against the approved roots (`PathGuard`: no `..`, junction/symlink escape, device paths).
* **No shell/process/filesystem routes exist.** Prompts, completions and tokens are never logged (request log =
  method, path, status, request id).
* Duplicate `Host`/`Authorization`/`Origin`/`Content-Type` headers are treated as invalid.

## Error format

Every error (auth, validation, engine) has the same body and an `X-Request-Id` header:

```json
{"error":{"message":"...","type":"invalid_request_error","code":"invalid_json","param":null,"request_id":"req-..."}}
```

| HTTP | `code` examples |
|---|---|
| 400 | `invalid_json`, `invalid_request`, `invalid_argument` |
| 401 | `unauthorized` (+ `WWW-Authenticate: Bearer`) |
| 403 | `host_not_allowed`, `origin_not_allowed`, `security_violation` |
| 404 | `not_found` |
| 405 | `method_not_allowed` (+ `Allow`) |
| 409 | `invalid_state`, `cancelled`, `already_exists` |
| 413 | `payload_too_large` |
| 415 | `unsupported_media_type` |
| 422 | `model_error` |
| 429 | `too_many_requests` (+ `Retry-After`) |
| 501 | `unsupported` |
| 502 / 503 / 504 | `backend_error`, `out_of_memory` / `overloaded` (both with `Retry-After`, see [overload-protection.md](overload-protection.md)), `timeout` |

(`code` for engine errors is the stable `hcs::ErrorCode` name from `include/hcs/result.hpp`.)

## OpenAI-compatible routes

### `GET /v1/models`
`{"object":"list","data":[{"id","object":"model","created":0,"owned_by":"local","hcs":{"state","quantization"}}]}`.
Image models are omitted.

### `POST /v1/chat/completions`
Body schema (enforced): `messages` (required, 1..128 of `{role: system|user|assistant, content: string | [{type:"text",text}]}`),
`model` (id; if it is not the loaded model it is **loaded first**, which can take seconds; omit to use the loaded model,
`409 invalid_state` if none), `max_tokens` / `max_completion_tokens` (1..4096, default 256), `temperature` (0..2),
`seed` (>= 0), `stream` (bool), `n` (only 1). Accepted but **ignored**: `top_p`, `frequency_penalty`,
`presence_penalty`, `stop`, `user`, `stream_options`. Non-text `response_format` -> `501`. Tool calling is implemented
(see "Tool calling" below); legacy `functions`, `tool_choice` `required` and a named-function `tool_choice` -> `501`.

**Image input (vision)**: a `user` message may carry one `{"type":"image_url","image_url":{"url":"data:image/png;base64,..."}}`
content part next to its text part(s). Only `data:image/{png,jpeg,gif,bmp};base64,` URIs are accepted: remote
(`http(s)://`), `file://` and bare paths are `400` (the runtime never fetches URLs or reads server paths named by a
request). At most **one image per request**, in a single `user` message optionally preceded by one `system` message;
the image needs a text prompt. Limits (enforced before any decoder runs): decoded image <= 32 MiB
(`ControlPlaneOptions::max_image_bytes`) and <= 16 Mpixel (`max_image_pixels`, read from the PNG/JPEG/GIF/BMP header);
the HTTP body cap still applies, so raise `--max-body-bytes` (e.g. `12582912`) for photos. Streaming and
cancellation behave exactly like text chat (a client disconnect cancels the vision job). The model must be a
multimodal GGUF whose `mmproj*.gguf` projector sits in the **same folder** (paired by name, or as the only projector
next to the only other model); it is loaded together with the model. A model without a projector answers `501`
`unsupported` ("has no vision projector"), never a text-only guess. `input_image` and other part types are `400`.

Non-streaming response: standard `chat.completion` object with `choices[0].message`, `finish_reason`
(`stop`|`length`), `usage`, plus `hcs.job_id`.

Streaming (`"stream": true`): `text/event-stream`, standard `chat.completion.chunk` events (first delta carries
`role`, then `content` deltas, a final chunk with `finish_reason` and `usage`, then `data: [DONE]`). Errors before the first
token are normal HTTP errors; later failures are sent as a `data: {"error":...}` event followed by `[DONE]`.
**Client disconnect cancels the generation job.**

#### Tool calling (`tools`, `tool_calls`)

**Experimental, implemented** in `src/api/chat_tools.cpp` (tested with LFM3-Omni-2B on CPU and Vulkan,
`docs/test-evidence/lfm3/`). Request: `tools` (<= 64 entries, <= 64 KiB, function names `[A-Za-z0-9_.-]{1,64}`, unique),
`tool_choice` `"auto"` (default when `tools` is present) or `"none"`, assistant messages with `tool_calls`
(string-encoded `arguments`, `content` may be `null`) and `tool` role messages with the results. The prompt follows the
model's chat template (`<tools>` block in the system turn; `<tool_call>` / `<tool_response>` turns), so the model
family must use that format; LFM3 is the verified one. Response: the generated text is parsed (XML form and Hermes JSON
form) into `choices[0].message.tool_calls` with schema-typed arguments and `finish_reason: "tool_calls"`; in streaming
mode a single `delta.tool_calls` chunk is sent and text that may start a `<tool_call>` block is held back so no tag text
leaks into `content`. Tool output cannot forge a turn (control tokens are neutralised). Limits seen in the evidence:
one call per turn; `tool_choice` `required` or a named function (nothing constrains the sampler) and legacy
`functions` answer `501`; a `tool` message without `tools` is `400`.

### `POST /v1/images/generations`, `POST /v1/images/edits`

Authenticated, **Experimental**. Real stable-diffusion.cpp output (HTTP route tested with SD1.5 Q8_0,
`docs/test-evidence/M3-image/`; FLUX.3 Klein renders are proven through the engine and CLI, see `docs/test-evidence/wave-f/`). The request is a JSON body (`Content-Type: application/json`); `multipart/form-data`
(what OpenAI's edit endpoint uses) is **not** supported and answers `415` with that hint.

`POST /v1/images/generations`

| Field | Meaning |
|---|---|
| `prompt` (required) | text prompt (<= `max_prompt_bytes`) |
| `model` | image model id (kind `image`); loaded first if it is not the loaded model. Omit to use the loaded image model (none loaded -> `409 invalid_state`) |
| `size` | `"<W>x<H>"`, default `512x512`; each in 64..2048 and a multiple of 8, W*H <= 1 Mpixel (`ControlPlaneOptions::max_generate_pixels`) |
| `steps` | 1..150, default 20 |
| `allow_low_quality` | boolean, default `false`. Without it a request below the model family's quality envelope is rejected with `400` and a message starting `low_quality_request:` (SD1.5: short side < 384 px or < 8 steps; FLUX.3 Klein: short side < 384 px or < 4 steps without a preset). A 256 px / 2-step SD1.5 render is a pipeline smoke test, not an image. The same field exists on the MCP image tools and as CLI `--allow-low-quality` |
| `preset` | `fast` \| `quality`: measured steps/cfg point of the loaded family (FLUX.3 Klein only) |
| `seed`, `negative_prompt`, `cfg_scale` (0..30, 0 = model default) | HCS extensions |
| `loras` | HCS extension: up to 4 `{"id","weight"?}` objects, ids from `GET /hcs/v1/loras` (paths are not accepted). `weight` is `0 < abs(w) <= 2`, default = the manifest's recommended weight. A LoRA for another base family answers `400` with a message starting `lora_incompatible:`; unknown id `404`; no LoRA library `501`. Checked before the model is touched, repeated by the engine against the loaded weights. See [`docs/lora.md`](lora.md) |
| `upscale` | HCS extension: `"off"` (default) or `"4k"`: tiled 4x ONNX super-resolution then Lanczos to a 3840 px long edge. A missing `onnxruntime.dll`/model answers `501` **before** the render starts. If the upscale itself fails the un-upscaled image is returned and `hcs.upscale = {"status":"failed","error":...}`. See [`docs/upscale.md`](upscale.md) |
| `preset` | `fast` or `quality`: replaces `steps` and `cfg_scale` with the measured point of the loaded model family. Only the FLUX.3 family has a table; any other family answers `400`/`InvalidArgument` (never silently ignored). HCS extension, see `docs/supported-models.md` |
| `n` | only `1` |
| `response_format` | only `b64_json` (the default here, unlike OpenAI); `url` -> `501` (nothing is hosted) |

Per-family defaults (`include/hcs/image_profiles.hpp`): SD1.5 = 512x512, 20 steps, sampler Euler a, cfg 7; FLUX.3 Klein =
512x512, 16 steps (or the measured `fast` = 6 / `quality` = 16 presets), cfg 1.0. The family is detected when the model is
loaded (and, for an early synchronous rejection, guessed from the model id); a model whose family is unknown has no envelope.

Response `200`: `{"created", "data":[{"b64_json":"<PNG>"}], "hcs":{"job_id","request_id","model","format","width","height","steps","generation_ms","seed?","preset?"}}`.

`POST /v1/images/edits` takes the same fields plus `image` (required): a `data:image/{png,jpeg};base64,...` URI or bare
base64 of a PNG/JPEG, and `strength` (0..1, default 0.75). `size` defaults to the input image's size. `mask`
(inpainting) -> `501`. Remote URLs and `file:` URLs are rejected (`400`): the server never fetches a URL or reads a
path named by a request. The decoded input is limited to 32 MiB / 16 Mpixel and is header-probed before any decoder
runs. The whole request body is bounded by `--max-body-bytes` (default 1 MiB: raise it for large edit inputs).

Behaviour shared by both routes:

- The request runs as a job (`image.generate` / `image.edit`) on the single engine worker; the response is sent when the
  render finishes. The diffusion engine adapter has a per-step progress sink, but this route does not expose it yet (no
  streaming, no progress events, no intermediate images). `GET /hcs/v1/jobs/{id}` (id from `hcs.job_id` in the response) shows the result summary.
- **Client disconnect cancels the render** (the handler probes the connection every 150 ms), the request timeout
  (`--timeout-s`) cancels it and answers `504`; `POST /hcs/v1/jobs/{id}/cancel` stops a job started elsewhere (e.g. MCP).
- Errors: invalid sizes/steps/images `400`, unknown model `404`, text model named for an image request `400`, no image model
  loaded `409`, out of memory `503`, engine failure `422`/`502`, no image engine linked `501`.
- Generating loads/unloads models through the control plane: using a text model afterwards unloads the image model
  (and vice versa), one model resident at a time.

## HCS-native routes

| Route | Result |
|---|---|
| `GET /hcs/v1/health` | no auth: `{"status":"ok","version"}` |
| `GET /hcs/v1/runtime` | engine availability/metrics, loaded model, pending jobs, uptime |
| `GET /hcs/v1/devices` | hardware/Vulkan report (from `hcs_hardware`'s doctor JSON; `{"status":"unknown",...}` without it) |
| `GET /hcs/v1/memory` | budget (`"unknown"` strings when no live measurement), reservations |
| `GET /hcs/v1/runtime/overload` | overload controller snapshot: level (`normal`/`pressure`/`critical`), live RAM/commit/GPU-heap sample (`"unknown"` where not measurable), in-flight jobs, counters, limits, last shed. `{"enabled":false}` when disabled. Never throttled |
| `POST /hcs/v1/runtime/shed` | kill switch: cancels every queued/running job and unloads the model within the shed deadline; returns `{reason,jobs_cancelled,model_unloaded,unload_pending,jobs_idle,duration_ms}`. Needs the bearer token; never throttled |
| `GET /hcs/v1/models`, `GET /hcs/v1/models/{id}` | manifests + lifecycle state (no file paths) |
| `GET /hcs/v1/loras` | image LoRA library: id, name, base family, trigger words, recommended weight, licence, source, sha256 (+ `sha256_status`), status, problems. Never file paths. Empty list + a note when no library exists |
| `POST /hcs/v1/models/{id}/load` | `202 {"job_id","status_url"}` (asynchronous job) |
| `POST /hcs/v1/models/{id}/unload` | `200`; cancels a running generation first; `409` if that model is not loaded |
| `GET /hcs/v1/jobs/{id}` | `{id,name,state,error,event_count,result?}`; states `queued running succeeded failed cancelled` |
| `POST /hcs/v1/jobs/{id}/cancel` | cancels (queued: immediately; running: cooperatively); returns the job |
| `POST /hcs/v1/benchmarks` | `202 {"job_id","status_url","suite","model"}`; asynchronous text benchmark, see "Benchmarks" below |

### Benchmarks

`POST /hcs/v1/benchmarks` runs the same runner as `HCSInfer benchmark` (`src/core/benchmark.cpp`) as a job. Body
(`Content-Type: application/json`, at most 4096 bytes, no unknown keys): `model` (required, a registered model id),
`suite` (`quick` default, or `full`), `max_tokens` (1..4096), `warmup` (0..20), `iterations` (1..50), `seed`,
`sweep_sizes` (1..8 integers of 16..1048576; `full` only). The answer is `202` with `job_id` and `status_url`; poll
`GET /hcs/v1/jobs/{id}` for progress and the result record (AGENTS.md 21.1 JSON; `full` adds `prefill_sweep` and
`decode_sweep`, points that do not fit the context are `SKIPPED`, a failed point makes `result_status` `PARTIAL`).
`POST /hcs/v1/jobs/{id}/cancel` stops it. Errors: unknown model `404`, malformed or invalid body or an image model
`400`, body over 4 KiB `413`, wrong content type `415`, no text engine linked `501`. A real-model run of this route is
NOT RUN (the contract tests in `tests/contract/api/test_http_benchmarks.cpp` run without a model).

Model states: `discovered validating ready_to_load loading loaded running unloading unloaded failed`.

## Chat prompt construction

`auto` (default): a single user message, optionally preceded by one system message, is passed to the engine with the
model's own chat template (`apply_chat_template=true`). Multi-turn conversations are rendered as ChatML
(`<|im_start|>role\n...<|im_end|>`) and sent verbatim - correct for ChatML models (Qwen, many others), **not** for other
template families. `chatml` / `plain` force local rendering. Engine adapters do not yet expose a multi-turn template API.

## Engine integration point

`include/hcs/engine.hpp` has no channel for generated text (`generate()` returns only a `JobId`), so the API layer
defines `hcs::api::ITextStreamEngine` (`include/hcs/api/text_stream.hpp`): a blocking `generate_stream(request, sink,
cancel_token)`. `src/api/llama_stream_adapter.cpp` implements it over `hcs::engines::LlamaTextEngine` using a private
single-worker `JobQueue` for the engine (the engine submits its own jobs; sharing the control-plane queue would
deadlock a one-worker queue).

## Verified evidence

See `docs/test-evidence/M3-api-mcp/` (contract tests; real-engine e2e results and their status).
