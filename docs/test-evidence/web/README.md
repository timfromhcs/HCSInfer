# Evidence: HCS Studio Web (`web/`, ADR-013, V1.1 wave 2)

Date: 2026-11-17. Machine: Windows 11 build 10.0.26200, Ryzen 7 7735HS, Radeon 680M, Node v22.23.1, npm 11.21.1,
Google Chrome (headless). Statuses: PASS / FAIL / BLOCKED / NOT RUN.

## What was run

| Gate | Status | Evidence |
|---|---|---|
| `npm test` - 46 unit + static security tests (Node's built-in runner, no dependencies) | **PASS 46/46** | this run; command below |
| `node tests/e2e/boot.mjs` - real static server + real headless Chrome | **PASS 14/14** | `web-boot.txt`, `web-boot.json` |
| Static server: CSP / `nosniff` / `frame-ancestors`, correct MIME, path-traversal containment | **PASS** | `web-boot.txt` (traversal -> HTTP 404) |
| Page loads, ES modules execute, capability probe runs | **PASS** | headless Chrome `--dump-dom`; device pill resolved to `WebAssembly (CPU)` |
| Screenshot of the rendered UI | **PASS** | `web-chat.png` |
| **Real WebGPU inference** (load a model, generate text/images in the browser) | **NOT RUN** | the headless browser has no WebGPU adapter, so the probe fell back to WASM. Needs a real WebGPU browser. |
| WASM-path inference (real model download + generation) | **NOT RUN** | the e2e checks the shell and the probe, not generation (would download hundreds of MB in CI) |
| Vercel deploy | **NOT RUN** | needs the project owner's Vercel credentials (external decision, AGENTS.md 26.6) |

## Commands

```powershell
cd web
npm test
node tests/e2e/boot.mjs --evidence ..\docs\test-evidence\web
```

Results (this run):

```
# npm test
# tests 46
# pass 46
# fail 0

# tests/e2e/boot.mjs
PASS  server responds on 127.0.0.1 - HTTP 200
PASS  Content-Security-Policy header present
PASS  X-Content-Type-Options header present
PASS  frame-ancestors denied
PASS  index.html has the app shell
PASS  app.css served as text/css
PASS  js/app.js served as javascript
PASS  path traversal is contained (cannot read outside the web root) - HTTP 404
PASS  headless browser loaded a DOM - C:\Program Files\Google\Chrome\Application\chrome.exe
PASS  app shell rendered (tabs present)
PASS  capability probe ran (device pill updated) - detected: WebAssembly (CPU)
PASS  chat model options populated
PASS  no server-side template markers leaked
PASS  screenshot captured
PASS: 14/14 checks
```

## What this proves and what it does not

* **Proves**: the static site builds with no toolchain, the ES-module graph resolves, the UI renders in a real
  browser, the capability probe distinguishes WebGPU from WASM at run time, the CSP/security headers are correct,
  the dev server cannot be walked out of its root, and the one third-party runtime is pinned and consistent across
  `js/config.js`, `index.html` and `vercel.json`/`tools/serve.mjs` (asserted by `tests/static.test.mjs`).
* **Does not prove**: that a model actually loads and generates in a browser. That is WebGPU/WASM inference and it is
  **NOT RUN** here. Per AGENTS.md 26.7 it must be done in a real WebGPU browser before any "verified in browser"
  claim. The evidence JSON records `webgpu_inference: "NOT RUN"` explicitly.

## CI observation (2026-11-17)

The non-blocking headless step in CI (run 44160553968, `web` job) reported 12/13: the capability probe had not
updated the device pill ("detected: probing device"). Cause: the runner's headless Chrome **does** expose
`navigator.gpu`, but `requestAdapter()` never settled, so the probe was still awaiting it when the DOM was dumped.
Fix: `probeCapabilities` now races the adapter request against a 4 s timeout and falls back to WASM (unit test
`probeCapabilities does not hang on a never-settling WebGPU adapter`). The next run (44164109317) reported the step
**13/13, device pill `WebAssembly (CPU)`**, so the headless step is now a **blocking** gate (the unit/static job was
already blocking).

## Files

* `web-boot.json` - machine-readable summary of the 14 checks (`status`, per-check results, `webgpu_inference`).
* `web-boot.txt` - the same, human-readable.
* `web-chat.png` - headless Chrome screenshot of the Chat tab (HCSmedia branding, dark theme, `WebAssembly (CPU)`
  device pill, model selector populated).

## Reproduce the WebGPU path (not run here)

Open the site in a Chrome/Edge build with WebGPU enabled, pick `onnx-community/Qwen2.5-0.5B-Instruct`, send a
message and record model load time, time to first token and total time from the Performance panel and the chat
banner. Only then replace "NOT RUN" above.
