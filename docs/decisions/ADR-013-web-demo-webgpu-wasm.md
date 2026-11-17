# ADR-013: HCS Studio Web (browser-only WebGPU/WASM demo)

Date: 2026-11-17 - Status: accepted (implemented in `web/`) - Owner: V1.1 wave 2 (AGENTS.md 26.7)

## Context

AGENTS.md 26.7 asks for a separate deliverable under `web/`: a static site that runs all inference in the user's
browser via WebGPU with a WASM fallback, with no server inference, no analytics on content, a strict CSP, Hugging
Face model selection in the browser, and feature parity with the local app where a WebGPU-runnable model exists
(of Chat, Vision, Image Studio, Models, Performance, Settings); anything that cannot run in a browser must be shown
as "Local only" with a reason, never faked. Deployment to Vercel is allowed only after build + browser tests pass,
and real WebGPU checks are NOT RUN unless done in a real WebGPU browser.

The native app (ADR-009) already ships an embedded vanilla HTML/CSS/JS workbench ("HCS Studio") served over the
loopback API. That UI cannot be reused verbatim here: it is an authenticated client of a local server, whereas the
web demo must do the inference itself and has no server.

## Decision

1. **Runtime: transformers.js** (`@huggingface/transformers`), pinned to **4.3.2** (Apache-2.0), loaded as an ES
   module from a pinned jsDelivr URL through an import map. It handles both text-generation and image-to-text in one
   runtime, uses WebGPU when present and WASM otherwise, applies each model's own chat template, and caches weights
   in the browser Cache API. The WASM operator runtime (onnxruntime-web) is a transitive dependency of it.
2. **No framework, no build step.** The demo is plain HTML/CSS/ES modules, in the same spirit and with the same
   design tokens as the embedded UI (HCSmedia branding, dark/light themes, WAI-ARIA tabs). The web package has
   **no npm dependencies**; the only third-party byte that reaches the browser is the pinned CDN module.
3. **Local-first privacy.** All inference runs in the tab. The page contacts only `huggingface.co` (model metadata
   and weights) and the pinned CDN. There is no analytics, no telemetry, no server inference, and no prompt/image
   content in any request. A Hugging Face token (only needed for gated models) is kept in memory/sessionStorage for
   that tab and is sent only to `huggingface.co`, through the documented `env.fetch` hook.
4. **Honest feature map.** Chat, Vision, Models, Performance and Settings run in the browser. Image Studio, Train,
   the shell-based Agent, the Model Vault and the Vulkan kernels are shown as "Local only" with the reason; the
   Image Studio tab contains no controls and no fake render.
5. **Security.** Strict CSP (`default-src 'none'; script-src 'self' 'wasm-unsafe-eval' https://cdn.jsdelivr.net;
   connect-src 'self' <hf hosts> <cdn>; ... frame-ancestors 'none'`), set as a response header by `vercel.json` and
   the dev server, mirrored by a meta tag for hosts without header control. No inline event handlers, no
   `innerHTML`/`eval`, no external fonts or images. The dev server normalises and bounds every path to the web root.
6. **Tests as the gate.** `web/tests/*.test.mjs` (Node's built-in runner, 45 tests) cover the pure logic
   (formatting, capability decision, catalog, Hub parsing, message building, settings) and assert the static
   security properties (CSP, single pinned runtime, no analytics, no injection sinks, dependency-free package).
   `web/tests/e2e/boot.mjs` boots the site on the real static server and in a real headless Chrome (14 checks).
7. **Deployment.** `web/vercel.json` is a static deploy (`buildCommand: npm test`, so a deploy cannot ship if the
   tests fail). The deploy itself is NOT RUN: it needs the project owner's Vercel credentials (external decision).

## Rejected alternatives

- **WebLLM** for chat plus transformers.js for vision: two runtimes and two model catalogs for one demo; rejected
  in favour of one runtime that covers both tasks.
- **Reusing the embedded UI assets directly**: they are a loopback-API client, not a self-contained inference app.
- **A framework / bundler (React, Vite)**: a build step and a large dependency tree for a static demo; the local
  UI is already dependency-free vanilla, so consistency favours the same approach.
- **Vendoring the runtime bytes into the repo**: adds ~10 MB of third-party minified code and an update burden;
  a version-pinned CDN URL with an asserted pin is easier to audit. (If offline/air-gapped use is needed later,
  vendoring is the migration path.)
- **Faking an Image Studio**: there is no proven browser-runnable diffusion path here, so the tab is "Local only".

## Risks / open items

- **Real WebGPU inference is NOT RUN in this environment.** The headless browser has no WebGPU adapter, so the
  boot test only proves the shell and the capability probe (it selected the WASM path). A real run on a WebGPU
  browser is required before any "verified in browser" claim.
- The `fast`/few-step or LoRA claims from V1.1 do not apply here; the demo inherits the native app's stance.
- Model quality of small (0.36B-0.6B) models is not a claim of this deliverable.
- If jsDelivr or the Hub changes availability, the pinned URL is the single point of update (in `js/config.js`,
  `index.html`, and `vercel.json`).

## Evidence

`web/` (source + `README.md`), `docs/test-evidence/web/` (`web-boot.json`/`.txt`, screenshot), ADR-013. The e2e
script records `webgpu_inference: "NOT RUN"` explicitly.
