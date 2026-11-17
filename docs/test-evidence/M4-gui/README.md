# M4 GUI evidence (embedded local web UI, ADR-009)

Date: 2026-11-17. Machine: Windows 11 (26200), Ryzen 7 7735HS, Radeon 680M. Build: `build/img-api`, Release,
**CPU-only** (`HCS_ENABLE_VULKAN=OFF`), MSVC 19.44 x64, Ninja, `/W4 /WX` on HCS targets, embedded web UI enabled
(`HCS_HAVE_GUI`, `src/gui` linked into `hcs_api`).

## PASS: hook wired into the server, checked over a real socket

`ctest -R e2e_gui_real` -> **Passed** (2.0 s) in the full 400-test suite
(`../M10-fixes/ctest-full-400-green.txt`). The run is `tests/e2e/gui_real/e2e_gui.py` against
`build/img-api/bin/HCSInfer.exe`; its JSON is `e2e-gui.json`: **23 checks, 0 failed, 1 NOT RUN**.

The same script against the **Vulkan build** (`build/windows-msvc-release-vulkan/bin/HCSInfer.exe`) is
`vulkan-e2e-gui.json`: **22 checks, 0 failed, 1 NOT RUN** - 22 rather than 23 because the "Models table lists
registered models" check only runs when a model is registered, and that run passed no model.

Phase 1 (UI over HTTP, real socket): `GET /` -> 200 `text/html`; strict CSP (`default-src 'none'`,
`script-src 'self'`, no `unsafe-inline`); `X-Frame-Options: DENY` + `Referrer-Policy: no-referrer`;
`/ui/app.js` and `/ui/app.css` served with the right content type and their own CSP; the page and both assets carry
no token; five traversal attempts (`/ui/../x`, `/ui/..%2f..%2fwindows`, `/ui/%2e%2e/x`, `/ui/a/b`, `/ui/.env`) are
rejected (400/404); `POST /` -> 405; `GET /v1/models` without a token -> 401 (the API still authenticates next to the
UI); foreign `Host` and foreign `Origin` -> 403 (DNS-rebinding/CSRF); the API call with the token -> 200.

Phase 2 (real browser, headless Chrome): the page renders (`<title>HCSInfer</title>`), connects to the API using the
**URL-fragment token**, lists the registered models, and the token is not left in the DOM.

## PASS: unit tests (`hcs_test_gui_assets`, 11 tests)

`ctest-gui-unit.txt` - `hcs_gui` + `hcs_test_gui_assets` built in an isolated configuration
(`-DHCS_EXCLUDE_MODULES=<everything except core/gui>`): 11/11 pass. They cover: CSP present and strict, every asset
served with the right type, the embedded bytes are identical to `src/gui/assets/*`, no external URL / inline code /
dangerous JS API, token from the fragment only, traversal and malformed paths rejected, method handling, non-UI paths
untouched. The same 11 `GuiAssets.*` tests also run inside the full suite (tests 130-140 of
`../M10-fixes/ctest-full-400-green.txt`), together with the 2 `ApiUi.*` contract tests (296-297) that check the hook
path through `ApiHandler`.

## PASS (manual, real inference, NOT repeatable-automated): UI against a real server

Earlier manual run (kept for reference, `ui_proxy_dev.py` harness): page connected, 10 models listed; Models tab
loaded `qwen3-0.6b-q4_k_m` (`server job model.load succeeded`, header pill `loaded: ...`); Chat produced a real
streamed answer (first token 102 ms, 182 tokens, CPU) and Stop cancelled the running job
(`GET /hcs/v1/jobs/job-4` -> `cancelled`); Vision analysed a PNG with SmolVLM2 + mmproj (answer `Red.`); Image Studio
showed the real server reason while the route was still `501`; Performance rendered backend/memory/devices/jobs
(backend `cpu`, 28.86 tokens/s, resident 414.9 MiB). Superseded for the image case by `../M3-image` (Image Studio now
reaches the real route).

## NOT RUN

* Phase 3 (python-`playwright` interaction: click Load, chat, press Stop, check Image Studio): **NOT RUN**, python
  `playwright` is not installed on this machine (the script reports it as a NOT RUN check, never a pass).
* Vulkan-failed-but-UI-starts is covered by code review only (the page is static and the API 501/503 states are
  rendered); not run against a real Vulkan failure.
* Image generation through the UI (Image Studio) with a real diffusion model in the UI itself; the route is proven in
  `../M3-image` (CPU and Vulkan) but the UI was not driven through it here.
* Narrow/wide browser matrix, light theme, and OOM state against a real out-of-memory.
