# ADR-007: API and MCP security boundaries

Date: 2026-11-17 - Status: accepted (M3) - Owner: M3-api-mcp

## Context
AGENTS.md 14.3/14.4/19.1/19.2 require a local HTTP API and an MCP server that never expose arbitrary code execution
or filesystem access, bind to loopback by default, and resist DNS rebinding and cross-origin browser calls. A control
plane facade did not exist; engines expose no channel for generated text.

## Decisions
1. **Transport: cpp-httplib v0.44.0** (MIT, pinned tag + commit, header-only, downloaded via FetchContent with its own
   CMake disabled so no OpenSSL/zlib/brotli is probed). It is confined to `src/api/http_server.cpp`, compiled with
   relaxed warnings; all policy lives in `ApiHandler` (framework-neutral, unit-testable without sockets).
   Rejected: a hand-written Win32 server (more code to audit for the same result); Boost.Beast (heavy dependency).
2. **Authentication:** bearer token mandatory on every route except `GET /hcs/v1/health` (liveness only, minimal
   body). Random 256-bit token per run from `std::random_device`, printed to stderr; `--token`/`HCS_API_TOKEN` override.
   The token is also required with `--lan`. Rejected: no-auth loopback (any local process or a rebinding page could drive
   the runtime), cookies (no browser UI in scope).
3. **DNS rebinding + CORS:** `Host` allow-list (loopback names; bind host; explicit names; IP literals only with
   `--lan`), `Origin` must be same-origin including port or allow-listed, no CORS headers at all. Checked before
   authentication so a rebinding page learns nothing.
4. **Resource bounds:** body cap enforced by the transport before the body is read, JSON depth 64, strict schemas,
   per-request token/prompt/message caps, concurrency cap with 429, timeout with job cancellation, bounded job queue,
   bounded MCP line size and concurrent calls.
5. **No ambient authority:** no routes/tools take file paths for models; models are ids resolved by the registry and
   re-checked by `PathGuard` at load time. MCP image/input paths go through `PathGuard` before any other processing.
   No shell, process, or generic file tools. Unsupported capabilities answer 501/`unsupported`, never fake output.
6. **Privacy:** requests are logged as method/path/status/request-id only; prompts, completions and tokens are never
   logged (tested).
7. **Cancellation is real:** HTTP client disconnect, HTTP cancel route, request timeout, MCP `notifications/cancelled`
   and MCP EOF all cancel the job token; the engine adapter stops decoding. A cancelled MCP request gets no response.
8. **MCP version:** dual-era (2026-07-28 stateless + 2025-11-25 handshake) over stdio only; Streamable HTTP is not
   implemented (the 2026-07-28 transport would need per-request header mirroring and conformance tests that do not exist).
9. **Engine streaming interface:** `ITextStreamEngine` lives in the API layer because `engine.hpp` has no text channel;
   the llama adapter bridge uses a private single-worker queue to avoid self-deadlock.

## Risks / limits
* `std::random_device` is a CSPRNG on MSVC (rand_s) and libstdc++/glibc but this is not guaranteed by the standard;
  revisit with `BCryptGenRandom` at hardening (M10).
* The token is an in-memory bearer secret on loopback; any process of the same user can read the process command line
  if `--token` is used (prefer the random token or the environment variable).
* Same-user processes that obtain the token have full API access; there is no per-client scoping in V1.
* `--lan` has no TLS; use only on trusted networks or behind a TLS proxy.
* Multi-turn prompts assume ChatML unless the engine offers a template API.
* Rate limiting is a global concurrency cap, not per-client.

## Evidence
Contract tests: `tests/contract/api` (36, handler), `tests/contract/api_http` (9, real sockets), `tests/contract/mcp`
(22), `tests/unit/api_schema` (7); real-engine e2e `tests/e2e/api_mcp_real/e2e_real.py`. See
`docs/test-evidence/M3-api-mcp/`.

## Addendum 2026-11-20: transport pin v0.37.2 -> v0.44.0 (wave G)
An OSV.dev query of the pinned commit listed 9 advisories for cpp-httplib v0.37.2, four of them in the server request
path (request smuggling via an unconsumed GET body, negative chunk-size DoS, header percent-decoding CRLF injection, and
X-Forwarded-For under a trusted-proxy configuration). v0.44.0 is the first tag containing those fixes. The pin moved in
its own change; the local `process_and_close_socket` access patch still applies and the transport test suite is
unchanged. Details and the four client/TLS advisories that remain (unreachable: server only, no TLS, no client, no
WebSocket) are in `docs/licence-audit.md` Findings 6.
