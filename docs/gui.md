# HCSInfer local web UI (M4, ADR-009)

Status: **Experimental**. `HCSInfer serve` serves the UI on the same loopback listener as the API: `GET /` and
`/ui/*` are answered by `ApiHandler::handle()` (hook wired, see "Wiring"), after the Host/Origin checks and before bearer
auth. Evidence: `docs/test-evidence/M4-gui/` (asset/unit tests, headless-browser boot) and
`docs/test-evidence/M3-image/` (hook over a real socket, Image Studio against the real diffusion route).

## What it is

A single-page UI written in vanilla HTML/CSS/JS (no framework, no build step, no CDN, no external fonts). The three
assets in `src/gui/assets/` are compiled into the binary as C++ byte arrays by `src/gui/embed_assets.cmake` (a CMake
custom command, so editing an asset rebuilds the generated `.cpp`). The UI is only a client of the public API: it
calls `/v1/*` and `/hcs/v1/*` with the bearer token, so auth, Host/Origin checks, path guard and limits all apply to it
exactly as to any other client (AGENTS.md 3.2).

| Section | Calls | Notes |
|---|---|---|
| Chat | `POST /v1/chat/completions` (SSE), `POST /hcs/v1/jobs/{id}/cancel` | streams tokens; Stop = job cancel, then connection drop; first-token time and token count shown; selecting a model that is not loaded makes the server load it first (banner says so) |
| Vision | same route with an `image_url` data-URI part | one image, PNG/JPEG/GIF/BMP, 32 MiB client check; server body cap (default 1 MiB) still applies and its 413 is explained |
| Image Studio | `POST /v1/images/generations` | real route (stable-diffusion.cpp). When the build has no image engine the route answers `501` and the banner shows **Unsupported** with the server's reason, never a fake image. Cancel drops the request, which cancels the render server-side (no job id is known to the client until the response arrives) |
| Models | `GET /hcs/v1/models`, `POST .../{id}/load`, `.../unload`, `GET /hcs/v1/jobs/{id}` | load is the real asynchronous job (polled every 500 ms) with a Cancel button wired to job cancel |
| Performance | `GET /hcs/v1/{runtime,memory,devices}` | auto-refresh (default 3 s, paused while the tab is hidden), plus jobs started from this page |
| Settings | local only | Simple/Advanced mode, refresh interval, forget token. Server-side settings (model roots, backends) are not editable here |

Simple mode shows model + message/prompt only. Advanced mode adds system prompt, temperature, max tokens, seed (Chat),
max tokens/temperature (Vision), steps/seed (Image), each section with a **Reset to defaults** button.

### States

Every panel has a live-region banner with one of: **Working/Loading** (busy), **Error**, **Out of memory** (HTTP 503 or
code `out_of_memory`; adds advice), **Unsupported** (HTTP 501), **Done**. A `429` is reported as "server busy". `401`
re-opens the token prompt. A header pill shows `connected` / `server unreachable` / `not authenticated` and the loaded
model. The page renders and shows the token prompt even if the engine or Vulkan failed (it is static).

### Token handling

Open `http://127.0.0.1:<port>/#token=<token>` (the fragment is never sent to the server and not logged). On load the
page reads the fragment, removes it from the address bar with `history.replaceState`, and keeps the token in memory
and `sessionStorage` (this tab only). A `?token=` query string is **never read** (it is stripped from the URL if
present). Without a token the page shows a password field. The token is only ever sent in an `Authorization` header to
the same origin, is not embedded in any asset and is not logged (`console.log` is not used).

### Security

`hcs::gui::serve` attaches to every UI response:
`Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:;
connect-src 'self'; font-src 'none'; media-src 'none'; object-src 'none'; frame-src 'none'; worker-src 'none';
base-uri 'none'; form-action 'none'; frame-ancestors 'none'`, plus `X-Frame-Options: DENY`, `Referrer-Policy:
no-referrer`, `Cross-Origin-Opener-Policy/Resource-Policy: same-origin`, `Permissions-Policy` (camera, mic, geolocation,
payment, usb off). `ApiHandler::handle()` already appends `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`
to every response. The JS uses only `textContent` (no `innerHTML`/`eval`), no inline handlers or styles, no
`XMLHttpRequest`/`WebSocket`/`EventSource` (all enforced by unit tests). Asset paths are whitelisted
(`[A-Za-z0-9._-]`, no leading dot, no `..`): anything else is `400`, unknown `404`, non-GET/HEAD `405`.

### Accessibility / responsive

Skip link, WAI-ARIA tabs with arrow/Home/End keys, labelled controls, `aria-live` banners and log, visible focus ring,
light/dark via `prefers-color-scheme`, reduced-motion respected, layout works from ~320 px width (tables scroll).

## C++ API

`include/hcs/gui/gui_assets.hpp`, library `hcs_gui` (no third-party dependency):

```cpp
std::optional<hcs::gui::GuiResponse> hcs::gui::serve(std::string_view method, std::string_view path);
```

Returns `nullopt` for paths outside `/` and `/ui/*`; otherwise status, content type, security headers and a body view
over static storage.

### Wiring (done)

* `src/CMakeLists` order: `gui` is listed before `api` in `HCS_MODULE_DIRS` (root `CMakeLists.txt`), and
  `src/api/CMakeLists.txt` links `hcs_gui` and defines `HCS_HAVE_GUI`.
* `src/api/http_api.cpp`, `ApiHandler::handle()`: after the Host and Origin checks and before bearer auth,
  `hcs::gui::serve(req.method, req.path)` answers UI paths (status, content type, CSP and the other security headers,
  body from static storage). UI requests do not count against `--max-inflight`; `Cache-Control: no-store` and
  `X-Content-Type-Options: nosniff` are appended as for every response. A HEAD response has an empty body.
* The token flow is unchanged: `http://127.0.0.1:<port>/#token=<token>` (fragment, never sent to the server).
  Tests: `tests/contract/api_image` (`ApiUi.*`: no token needed for the assets, Host/Origin still enforced, POST -> 405,
  API routes still 401, page does not contain the token) and `tests/e2e/image_real/e2e_image.py` over a real socket.

## Tests

* `tests/unit/gui` (`hcs_test_gui_assets`, 11 tests): CSP present and strict, every asset served with the right type,
  embedded bytes identical to the source files, no external URLs / inline code / dangerous JS APIs, token from fragment
  only, traversal and malformed paths rejected, method handling, non-UI paths untouched.
* `tests/e2e/gui_real/e2e_gui.py`: boots `HCSInfer serve`, checks the UI over HTTP, then a headless Edge/Chrome boot
  check (`--dump-dom`), then python-playwright interaction (load, chat, stop) when available.
* `tests/e2e/gui_real/ui_proxy_dev.py`: **development harness only**. Serves the assets and proxies everything else to a
  running `HCSInfer serve`, so the UI can be tried against real inference before the hook is wired.

## Known limitations

* Image Studio sends `model, prompt, n, size, response_format=b64_json, steps, seed` and reads `data[0].b64_json`, which
  is exactly what `/v1/images/generations` implements. There is no progress bar (the engine reports none) and image
  edit is not in the UI (the route and the MCP tool exist). The size menu offers 256/384/512 squares only.
* No progress percentage for model load (the job API exposes state only), no jobs list endpoint (Performance shows
  jobs started from this page plus `jobs_pending`).
* Model `kind` is reported as `text` for vision models today, so the Vision selector lists text models too; the server
  answers 501 for a model without projector. Projector files (`mmproj*`) are hidden from the selectors.
* Chat history is kept in the page only (lost on reload); multi-turn uses the server's ChatML rendering.
