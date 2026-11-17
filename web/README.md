# HCS Studio Web

A browser-only demo of the HCSInfer control plane (AGENTS.md 26.7). All inference runs in **your** browser tab via
**WebGPU**, falling back to **WebAssembly** when there is no GPU. Nothing is uploaded, there is no server inference,
and there are no analytics.

Status: **Experimental**. The static site, the capability probe and the UI are proven in a real headless Chrome
(`docs/test-evidence/web/`). **Real WebGPU inference is NOT RUN in this repository's environment** (headless Chrome
has no WebGPU adapter); run it yourself on a WebGPU browser before trusting that path.

## Run it locally

No build step and no npm dependencies:

```powershell
node tools/serve.mjs 8080
# open http://127.0.0.1:8080/
```

The dev server serves this folder with the same security headers that production uses.

## What runs here

| Section | Runs in the browser | Notes |
|---|---|---|
| Chat | yes | text-generation model via transformers.js, streamed tokens, stop |
| Vision | yes | image-to-text model, one image per request, read locally |
| Models | yes | curated catalog, Hub search, model metadata (licence, size, gating), browser cache |
| Performance | yes | WebGPU adapter, WASM features, CPU threads, per-run timings |
| Settings | yes | theme, advanced options, forget token, clear browser model cache |
| Image Studio | **no - "Local only"** | needs a diffusion runtime; see the native app |
| Train, Agent, Model Vault, Vulkan kernels | **no - "Local only"** | they need the native app |

## Privacy and security

* All inference is in the tab. The only hosts contacted are `huggingface.co` (model metadata and weights) and the
  pinned CDN (`cdn.jsdelivr.net`) for the one third-party runtime.
* Strict CSP (`default-src 'none'`, `script-src 'self' 'wasm-unsafe-eval' <cdn>`, `frame-ancestors 'none'`), no
  inline handlers, no `innerHTML`/`eval`, no external fonts or images.
* A Hugging Face token is needed **only** for gated models. It is kept in memory/sessionStorage for this tab and is
  sent only to `huggingface.co`. "Forget token" clears it.

## The one third-party dependency

`@huggingface/transformers` **4.3.2** (Apache-2.0), pinned in three places that a test asserts are identical:
`js/config.js`, the import map in `index.html`, and the CSP in `vercel.json`/`tools/serve.mjs`. Its transitive WASM
runtime is `onnxruntime-web`. The web package itself has no npm dependencies.

## Tests

```powershell
npm test                     # 45 unit + static security tests (Node's built-in runner)
node tests/e2e/boot.mjs      # real static server + real headless Chrome (14 checks)
node tests/e2e/boot.mjs --evidence ..\docs\test-evidence\web
```

`npm test` also runs as the Vercel `buildCommand`, so a deploy cannot ship if the tests fail.

## Deploy (Vercel)

`web/vercel.json` is a static deployment. Deployment is **NOT RUN** here (it needs the owner's Vercel credentials):

```powershell
# from the repository root, with the Vercel CLI logged in:
vercel deploy web --prod
```

Set the Vercel project root to `web/`. The `buildCommand` is `npm test`; model weights are delivered by the Hub to
the visitor's browser and are never part of the deployment.
