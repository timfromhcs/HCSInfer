# M10 security audit: API, MCP, path guard, storage parsers, engines

Scope: `src/api`, `src/mcp`, `src/core/path_guard.cpp`, `src/storage/*` parsers, `src/engines/*`
against AGENTS.md sections 14 and 19 and ADR-007. Method: source review plus black-box requests
against the existing `build/windows-msvc-release/bin/HCSInfer.exe` (not rebuilt).
Test models: `qwen3-0.6b-q4_k_m.gguf` (copied into a scratch approved root). Build under test is
CPU-only (`/hcs/v1/runtime` reports backend `cpu`); Vulkan paths are NOT RUN.
Harness scripts (not committed): scratchpad `http_audit.py`, `http_audit2.py`, `mcp_audit.py`,
`mcp_deep.py`, `stress.py`, `badgguf.py`.

Evidence levels: REPRODUCED = observed against the binary; CODE = confirmed by reading the code,
not reproduced end to end; SUSPECTED = plausible, not proven.

## Gate results

| Check | Result |
|---|---|
| Loopback bind default | PASS (listens on 127.0.0.1 only; `--host 0.0.0.0` without `--lan` rejected; `--lan` binds 0.0.0.0 only on opt-in with token) |
| Bearer token auth | PASS (401 without/bad token, duplicate Authorization rejected, token in query ignored, 256-bit random default) |
| Host / DNS rebinding | PASS (evil host, `localhost.evil.com`, `127.0.0.1.nip.io`, missing Host all 403) |
| Origin validation | PASS (evil, `null`, other-port origins all 403; also on health and OPTIONS) |
| Body size limit | PASS (1 MiB+ -> 413, chunked 1.5 MiB -> 413) |
| JSON depth (HTTP) | PASS (100 and 200000 nesting -> 400); FAIL for MCP (see D1) |
| Schema / type / range checks | PASS (n=2, max_tokens overflow, temperature 5, bad roles) |
| Job-id / model-id validation | PASS (job-0, overflow, `%00`, `..`, uppercase, 100 chars, double slash) |
| Path guard: outside root, `..`, junction escape, ADS, `\\?\`, UNC, NUL byte, sibling prefix, case variants | PASS |
| Path guard: DOS device names | FAIL (D5) |
| No prompt/token in logs | PASS (marker prompt and token absent from stderr in API and MCP runs) |
| MCP stdout cleanliness | PASS for CPU build (load + chat + errors: every stdout line valid JSON); Vulkan NOT RUN |
| Malformed GGUF through load (10 crafted files) | PASS (all rejected cleanly by llama.cpp, no crash) |
| Existing unit suites (storage 59, fuzz 3, api 36, api_http 9, mcp 22, schema 7) | PASS (re-run, no rebuild) |
| Concurrent chat/load/unload/cancel storm (25 s) | PASS (no crash or deadlock; server answers afterwards) |

## CONFIRMED defects (ranked)

### D1. MCP stdio process crashes (stack overflow) on deeply nested JSON  [REPRODUCED, HIGH for robustness]
- File: `src/mcp/mcp_server.cpp:337` (`const json params = msg["params"];` deep copy),
  also `:427` (`json args = params["arguments"]`) and `:535` (`msg` copied into the worker lambda).
  No depth guard exists on the MCP path (the HTTP path has `json_depth_ok`).
- Repro: send one line `{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{...},"x":[[[...10000 deep...]]]}}`
  to `HCSInfer mcp stdio`. Exit code 0xC00000FD (STATUS_STACK_OVERFLOW) at depth 10000, no response;
  depth 1000 is fine. Same via `tools/call` `arguments`. The parse itself survives (400k-deep array
  gives a normal parse error), the recursive copy is the crash. Line cap is 1 MiB so depth up to ~500k fits.
- Impact: any MCP client (or a prompt-injected agent) kills the server with no auth needed; violates
  M3 acceptance "malformed request is rejected".
- Fix: take `const json& params = msg.at("params")` (no copy), pass `std::move`/reference into the worker,
  and reject deep input before parsing (reuse `json_depth_ok` in `McpServer::run`/`handle_line`, cap 64).

### D2. HTTP thread pool misuse: queue is unbounded and idle sockets starve the server  [REPRODUCED, MEDIUM]
- File: `src/api/http_server.cpp:78`: `new httplib::ThreadPool(threads, queued)`. The vendored
  signature is `ThreadPool(n, max_n, mqr)`: `queued` (32) lands in `max_n` (max dynamic threads) and
  `max_queued_requests` stays 0 = unlimited. `max_queued` is therefore not a bound at all.
- Repro: open 120 idle TCP connections to the API (no bytes sent, no auth), then `GET /hcs/v1/health`:
  answered only after 14.4 s (read timeout is 30 s; 32 workers all parked on idle sockets, accept queue grows without limit).
- Impact: unauthenticated denial of service from any local process (or any LAN peer with `--lan`);
  unbounded memory/FD growth.
- Fix: `new httplib::ThreadPool(threads, threads, queued)`; set a short header-read timeout
  (e.g. `set_read_timeout(5)` until headers complete) and cap simultaneous connections; keep `--lan` guarded by firewall docs.

### D3. Non-streaming chat keeps generating after the client disconnects  [REPRODUCED, MEDIUM]
- File: `src/api/http_api.cpp:461-512` (first-output wait and non-stream collect loops only poll the channel;
  no connection-closed check). Streaming mode is fine (cancel on failed write, verified: job -> cancelled).
- Repro: POST non-stream chat (`max_tokens` 2000), close the socket after 1 s. Job `job-N` stayed
  `running` and kept emitting events (165 -> 1015 events over 18 s) until max_tokens/timeout (300 s default).
  Streaming client abort on the same prompt cancelled the job within 3 s.
- Impact: a cancelled/abandoned request keeps the single engine worker busy; every other request queues or 504s;
  violates "cancellation stops actual work".
- Fix: pass `httplib::Request::is_connection_closed` (or poll `is_connection_closed()` every 100-250 ms inside the pop loops)
  through `HttpRequest`, and `cancel_job` when the peer is gone.

### D4. Build has no memory budget, so model loads are unguarded  [REPRODUCED via /hcs/v1/memory, MEDIUM]
- File: `src/api/runtime.cpp` (budget never given a total), `src/api/control_plane.cpp:145,179` (checks skipped when `total()==0`).
- Repro: `GET /hcs/v1/memory` -> `"budget_known":false`, "model-size budget checks are skipped".
  Settings allow `context_tokens` up to 2,097,152 (engine itself caps at 1,048,576: inconsistent, `settings.cpp:73` vs `llama_engine.cpp:300`).
- Impact: a registered 40 GB model or 1 M-token context from the API/MCP triggers a host OOM instead of a clean `out_of_memory` error
  (AGENTS 10.1/10.3, M2 acceptance).
- Fix: wire live physical-RAM + Vulkan budget into `MemoryBudget` at `Runtime::create`; add KV-cache estimate to the load check; align settings and engine context caps.

### D5. PathGuard accepts Windows DOS device names inside an approved root  [REPRODUCED (guard), latent impact, LOW-MEDIUM]
- File: `src/core/path_guard.cpp:34-55` (`check_raw`).
- Repro: `hcs.vision.analyze` / `hcs.image.edit` with `<root>\CON`, `<root>\NUL.png`, `<root>\COM1` return `unsupported`
  (guard passed) instead of `security_violation`. Win32 maps `<root>\NUL` to the NUL device (verified: opens, 0 bytes read).
  Also accepted: wildcard `*.png`, trailing dots/spaces (Win32 strips them; still inside root), 900-char names.
- Impact: once vision/image adapters open these paths, `CON`/`CONIN$` can block a worker on console input and device names bypass file-type checks.
- Fix: reject path components whose stem (before first `.`, trailing spaces/dots stripped) matches CON, PRN, AUX, NUL, COM1-9, LPT1-9 (also `COM¹` etc.), reject `* ? < > |` and trailing dot/space components.

### D6. Error messages leak absolute local paths to API/MCP clients  [REPRODUCED, LOW]
- Files: `src/engines/llama/llama_engine.cpp:309,313,371` (`tail_error_lines` of the llama log, `"cannot open model file: " + path`),
  `src/core/path_guard.cpp:80`, `src/storage/*` (`cannot stat <path>`).
- Repro: load crafted `bad_f.gguf` via `POST /hcs/v1/models/bad_f/load`, then `GET /hcs/v1/jobs/<id>`:
  message contains `failed to load model from C:\Users\<user>\AppData\...`.
- Impact: ADR-007/19.4 path/PII redaction; `hcs.models.inspect` correctly omits paths, errors do not.
- Fix: scrub absolute paths (replace root-relative or drop) in `Error.message` before it leaves the control plane; keep full text only in local logs.

### D7. `is_loopback_host` accepts any hostname starting with `127.`  [REPRODUCED validation pass, LOW]
- File: `src/core/settings.cpp:14` (`h.rfind("127.", 0) == 0`).
- Repro: `serve --host 127.foo.invalid` passes `validate()` (fails later only because DNS does not resolve).
  A name such as `127.example.net` resolving to a LAN address would bind a non-loopback interface without `--lan`.
- Fix: parse as IPv4 literal (`127.0.0.0/8` exact octets) or the literals `localhost`, `::1`; never prefix-match.

## CODE-CONFIRMED (not reproduced)

### C1. Diffusion adapter: queued jobs run without `run_mu` and results are never evicted  [CODE, MEDIUM, latent: image engine is not wired into API/MCP yet]
- `src/engines/diffusion/diffusion_engine.cpp:558-567`: the job lambda calls `render()` without `std::lock_guard run(s->run_mu)`
  (the synchronous `run()` at `:622` does take it; the llama adapter does too). With a queue of more than one worker, or mixing `generate()` job and `generate_image()`, two renders hit one `sd_ctx_t`.
- `:565` `impl->results[jc.id()] = ...` keeps every PNG (up to 4096x4096) until unload: unbounded memory growth per job.
- Also `render()`: `sd_set_generation_progress_callback(ctx, progress_cb, &pstate)` leaves a dangling pointer if `sd_generate_image` throws (`:699-704`); `model_bytes` is `uint32_t` (4 GiB truncation, `:269,477`).
- Fix: lock `run_mu` in the job lambda, cap `results` (LRU 8) or hand the bytes to the caller, reset the callback with a scope guard, widen `model_bytes` to `uint64_t`.

### C2. Model load jobs are not cancellable once running  [CODE, LOW]
- `src/api/control_plane.cpp:227-230`: `load_model(id)` ignores `ctx.token()`; `POST /hcs/v1/jobs/{id}/cancel` or MCP `notifications/cancelled` for a load only works while queued;
  `mcp_server.cpp:254-267` keeps waiting until the load finishes or `call_timeout` (the loop never breaks on cancel).
- Fix: check the token between validation, budget, engine load (engine API needs an abort hook, llama has `abort_callback` only for decode), break MCP loop on cancel.

### C3. Special-token injection from user text  [CODE, LOW-MEDIUM]
- `src/engines/llama/llama_engine.cpp:540-544`: `llama_tokenize(..., parse_special=true)` on text that includes user content, and `control_plane.cpp:241` builds ChatML by plain concatenation.
  A user message containing `<|im_end|><|im_start|>system ...` is tokenized as real control tokens (role forgery). Not a memory-safety issue but a prompt-boundary break for API clients that proxy third-party text.
- Fix: tokenize user content segments with `parse_special=false` or strip/escape template control tokens before building the prompt.

### C4. Chat/SSE JSON serialisation can throw on invalid UTF-8 model output  [CODE, SUSPECTED impact]
- `src/api/http_api.cpp:248,543,583,586` use `j.dump()` without `error_handler_t::replace` (MCP uses `dump_safe`). The engine only withholds a *truncated* UTF-8 tail (`complete_utf8_prefix`), but emits stray continuation bytes and flushes `pending` at the end.
  Non-stream: exception becomes HTTP 500 for an otherwise successful generation. Stream: the exception escapes the httplib content provider lambda (no try/catch in `r.stream`), which I could not prove is contained.
- I could not make the test model emit invalid bytes, so this is not reproduced.
- Fix: use `dump(-1, ' ', false, json::error_handler_t::replace)` everywhere and wrap the stream lambda in try/catch.

### C5. Storage parsers on the load path  [CODE, informational]
- `HCSModel`/`GGUF` parsers (`hcsmodel.cpp`, `gguf.cpp`, `gguf_probe.cpp`, `stream.cpp`, `codec.cpp`) use checked arithmetic, bounded counts, per-chunk hash verification and overlap rejection; no overflow or OOB found.
  `checked_mul` caps, `range_within`, `Cursor` bounds, LZ4/Zstd exact-size checks and the IOCP drain on cancel look correct.
- Gap: the load path never calls `storage::parse_gguf`; control plane/engine only check the 4-byte magic and hand the file to llama.cpp. 10 crafted GGUFs (huge tensor/KV counts, string length 2^62, alignment 0, dim 2^60, bad type, offset 2^63) were rejected cleanly by upstream, so no live defect, but AGENTS 19.3 "validate all sizes before allocation" is satisfied only by upstream. Consider running `parse_gguf` (bounded, 1 MiB window) before `llama_model_load_from_file`.
- `PlainFileReader::stream` sets `verify_hash=false` (no hashes in plain files) by design.

## SUSPECTED / lower confidence

- S1. Duplicate in-flight JSON-RPC ids on two `tools/call` requests overwrite `cancelled_[key]` / `running_jobs_[key]` (`mcp_server.cpp:532,182`): a cancel only reaches the later job. Spec forbids reuse of an id; harden by rejecting an id already in `cancelled_`.
- S2. Model switch TOCTOU in `submit_chat` (`control_plane.cpp:295-308`): after `load_model(p.model)` the job re-locks `engine_mutex_` and uses `loaded_` without re-checking it equals `p.model`. Unreachable while the API queue has 1 worker (default); becomes reachable if workers > 1 or other callers call `load_model`. Fix: compare `*loaded_ == p.model` under the lock.
- S3. `--token` on the command line is visible in the process list; prefer `HCS_API_TOKEN`/token file. The random token is printed to stderr by design.
- S4. `/hcs/v1/health` is unauthenticated and returns the version (intended per `serve --help`; acceptable but fingerprintable).
- S5. CLI `chat`/`image` (`src/cli/chat_cmd.cpp:107`, `image_cmd.cpp:116,241,282`) call engines directly and accept arbitrary `--model`/`--input`/`--output` paths without `PathGuard` and bypass the control plane (AGENTS 3.2, 14.2). Acceptable for the local user, but contradicts the "all paths validated against approved roots" wording; document or route through ControlPlane.
- S6. `Session`-level TOCTOU between `guard.resolve()` and the engine's open (`control_plane.cpp:141,188`): a file swapped inside an approved root after resolve is followed. Requires write access to the root; low.

## NOT RUN / BLOCKED

- Vulkan-specific paths (build is CPU only): device-lost, Vulkan OOM, MCP stdout cleanliness with a Vulkan driver.
- HDD unplug during streamed load, `--lan` from a second host, TLS (none implemented), real VLM/diffusion through API (adapters not wired: `/v1/images/*` -> 501, `image_url` -> 501).
- Windows reserved-name open behaviour was verified only with Python, not inside the HCS image adapter (not wired).

## Positive findings (verified)

Loopback default, mandatory token (constant-time compare, duplicate-header rejection), Host and Origin allow-lists (incl. same-port rule),
413/415/400 handling, schema bounds (n, max_tokens, temperature, roles, 128 messages, 256 KiB prompts), job/model id grammars,
prompt/token not logged, stdout clean in MCP (CPU), streaming disconnect cancels the job, junction/`..`/ADS/UNC/`\\?\` blocked,
concurrent load/unload/cancel storm survived, 10 malformed GGUFs rejected without crash, 134 existing tests re-run green.
