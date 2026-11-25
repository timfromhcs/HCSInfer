# HCS Studio: the embedded web UI (M4, ADR-009, AGENTS.md 26.5)

Status: **Experimental**. `HCSInfer serve` serves the UI on the same loopback listener as the API: `GET /` and
`/ui/*` are answered by `ApiHandler::handle()` (hook wired, see "Wiring"), after the Host/Origin checks and before bearer
auth. Evidence: `docs/test-evidence/M4-gui/` (asset/unit tests, headless-browser boot) and
`docs/test-evidence/M3-image/` (hook over a real socket, Image Studio against the real diffusion route).

## What it is

HCS Studio is a single-page workbench in vanilla HTML/CSS/JS (no framework, no build step, no CDN, no external fonts or
images). It has its own HCSmedia identity (inline-SVG mark and wordmark, a teal/ink token palette with a dark variant,
system font stack) and a layout in the spirit of a model workbench: left sidebar navigation, a top bar with connection and
loaded-model pills and a theme switch, and one work area. No third-party UI assets or code are used. The three assets in
`src/gui/assets/` are compiled into the binary as C++ byte arrays by `src/gui/embed_assets.cmake` (editing an asset
rebuilds the generated `.cpp`). The UI is only a client of the public API: it calls `/v1/*` and `/hcs/v1/*` with the
bearer token, so auth, Host/Origin checks, path guard and limits apply exactly as to any other client (AGENTS.md 3.2).

| Section | Calls | Notes |
|---|---|---|
| Chat | `POST /v1/chat/completions` (SSE), `POST /hcs/v1/jobs/{id}/cancel` | streams tokens; Stop = job cancel, then connection drop; first-token time and token count; selecting a model that is not loaded makes the server load it first (banner says so). Streamed `delta.tool_calls` are shown as expandable blocks (name and arguments); they are displayed only and never executed |
| Image Studio | `POST /v1/images/generations` (text to image), `POST /v1/images/edits` (image to image) | model, prompt, size, steps with presets (Draft 4, Fast 8, Standard 16, Fine 28), seed (empty = random, always sent so a result is reproducible), strength for image to image, drag-and-drop or file input for the input image, negative prompt and guidance scale in Advanced. Result panel with Save PNG, Use as input image, Copy seed; session gallery (24 images, in memory only). While rendering: elapsed seconds and an indeterminate bar. When the build has no image engine the server answers `501` and the banner shows **Unsupported** with its reason, never a fake image |
| Vision | same chat route with an `image_url` data-URI part | one image, PNG/JPEG/GIF/BMP, 32 MiB client check; server body cap (default 1 MiB) still applies and its 413 is explained |
| Models | `GET /hcs/v1/models`, `POST .../{id}/load`, `.../unload`, `GET /hcs/v1/jobs/{id}`, `GET /hcs/v1/memory` | load is the real asynchronous job (polled every 500 ms) with Cancel wired to job cancel. **Memory split** bar: budget entries grouped by category against the budget total, free remainder, and measured available physical memory. Unknown totals stay "unknown". Folders are registered on the server (`HCSInfer models scan <dir>`); there is no scan endpoint yet, so the button is **Refresh list** and the page says so |
| Performance | `GET /hcs/v1/{runtime,memory,devices}` | auto-refresh (default 3 s, paused while the tab is hidden), plus jobs started from this page |
| Settings | local only | Simple/Advanced mode, appearance (system/light/dark), refresh interval, forget token. Server-side settings (model roots, backends) are not editable here |
| Agent, Train | none | **Planned** (labelled in the sidebar and on the page). They show the real roadmap status (AGENTS.md 26.3, 26.4) and contain no controls at all, enforced by a unit test |

Simple mode shows model + message/prompt (+ the image fields that are always useful). Advanced mode adds system prompt,
temperature, max tokens, seed (Chat), max tokens/temperature (Vision), negative prompt and guidance (Image), each section
with a **Reset to defaults** button.

The step presets only set the step count. The UI makes no LoRA or "fast model" claim: few-step quality needs a
step-distilled checkpoint (AGENTS.md 26.1) and there is no LoRA control in the image API.

### Wave C polish (2026-11-20)

* **Safe Markdown in replies.** Chat and Vision answers are rendered by a small parser that builds DOM nodes only
  (headings, bold, italic, inline code, lists, quotes, rules, fenced code blocks, `http(s)` links with
  `rel="noopener noreferrer nofollow"`). Model output is never parsed as HTML; `<img onerror>` / `<script>` text from a
  model is shown as text. An unclosed code fence while streaming renders as code until it closes.
  Regression test in CTest: `e2e_gui_xss` (`tests/e2e/gui_real/studio_xss.py`) serves the real UI assets with a **fake** hostile
  model reply (`<img onerror>`, `<script>`, `<svg onload>`, `javascript:` link, `<iframe>`, unclosed fence) to a headless Edge/Chrome and
  asserts no element, no handler, no CSP violation. It needs a browser and the python `websockets` package (else SKIP = NOT RUN;
  `HCS_E2E_PYTHON_PATH` points at a dev venv). It proves UI sanitisation only; `studio_md.py` is the real-model variant.
* **Copy** buttons on every message, every code block, the Vision answer and (Image Studio) seed and prompt. The
  async clipboard is tried first, then a transient selection; a toast (aria-live) says whether it worked.
* **First-run guide** on the Chat tab: models registered, a device/memory summary read from `/hcs/v1/devices` and
  `/hcs/v1/memory`, and the Simple/Advanced choice. It hides after "Got it" or the first successful reply; Settings has
  "Show the welcome guide again" and "Reset all UI settings".
* **Performance Center**: measured tokens per second and first-token time of the last runs (client-side, from the stream's
  usage), a memory split bar with free/reserved/measured values, a curated runtime card, CPU / GPU / software-renderer
  device cards, and the raw device JSON behind a disclosure. No synthetic numbers: before any generation it says so.
* The log is `role="log"` and `aria-busy` while streaming so screen readers announce a finished reply, not every token.
* Contrast: every text/background token pair in both themes is >= 4.5:1 (computed from the tokens, light and dark).

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
to every response. The JS uses only `textContent` and DOM node construction (no `innerHTML`/`eval`), no inline handlers or styles, no
`XMLHttpRequest`/`WebSocket`/`EventSource` (all enforced by unit tests). Asset paths are whitelisted
(`[A-Za-z0-9._-]`, no leading dot, no `..`): anything else is `400`, unknown `404`, non-GET/HEAD `405`.

### Accessibility / responsive

Skip link, WAI-ARIA vertical tabs with arrow/Home/End keys, labelled controls, `aria-live` banners and log, visible focus
ring, reduced-motion respected. Colours are tokens on `:root` with a dark set under `prefers-color-scheme` (guarded by
`:root:not([data-theme="light"])`) and under `[data-theme="dark"]`; the header button and Settings switch between system,
light and dark (stored in `localStorage` as a convenience, the page works without it). Below 860 px the sidebar becomes a
slide-over opened by the menu button (Escape or choosing a tab closes it); tables scroll inside their own container and
the page never scrolls horizontally (checked at 1360, 820 and 390 px by `studio_shots.py`).

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

* `tests/unit/gui` (`hcs_test_gui_assets`): CSP present and strict, every asset served with the right type, embedded bytes
  identical to the source files, no external URLs / inline code / dangerous JS APIs, token from fragment only, traversal
  and malformed paths rejected, method handling, non-UI paths untouched, and `StudioStructure` (all eight tabs present,
  the Agent and Train panels contain no controls, image edits route and tool-call display wired, dark tokens present).
* `tests/e2e/gui_real/e2e_gui.py`: boots `HCSInfer serve`, checks the UI over HTTP, then a headless Edge/Chrome boot
  check (`--dump-dom`), then python-playwright interaction (load, chat, stop) when available.
* `tests/e2e/gui_real/studio_shots.py`: drives headless Edge over the DevTools protocol (needs the `websockets` package)
  against a running server: layout checks (no horizontal scroll on any tab at three widths), the Planned tabs, real model
  load through the Models tab, streamed chat plus Stop, text to image and image to image on a real diffusion model, and a
  screenshot of every tab at desktop and phone width in light and dark. Output and results: `docs/test-evidence/studio/`.
* `tests/e2e/gui_real/ui_proxy_dev.py`: **development harness only**. Serves the assets from disk and proxies everything
  else to a running `HCSInfer serve`, so the UI can be edited and tried without rebuilding.

## Known limitations

* No per-step image progress: the image routes are single blocking requests and do not expose the engine adapter's per-step progress sink yet, so the UI shows
  elapsed seconds and an indeterminate bar. Cancel drops the request, which cancels the render server-side. Per-step
  progress needs a streaming image route (API change, not part of the GUI).
* No folder scan or settings endpoint: Models can load, unload and refresh, but folders are added with the CLI.
* No progress percentage for model load (the job API exposes state only), no jobs list endpoint (Performance shows jobs
  started from this page plus `jobs_pending`).
* Model `kind` is reported as `text` for vision models today, so the Vision selector lists text models too; the server
  answers 501 for a model without projector. Projector files (`mmproj*`) are hidden from the selectors.
* Chat history and the image gallery are kept in the page only (lost on reload). Tool calls are displayed, not executed.
* Image sizes offered: 256/384/512/640/768 squares, 512x768 and 768x512, and "match input image" for image to image; the
  server's pixel budget decides what is accepted and its error is shown.
