# M3 image routes (HTTP + MCP) - real-model evidence

Date: 2026-11-17. Machine: Windows 11 (26200), Ryzen 7 7735HS, Radeon 680M. Build: `build/img-api`, Release,
**CPU-only** (`HCS_ENABLE_VULKAN=OFF`), MSVC 19.44 x64, Ninja, `/W4 /WX` on HCS targets, llama.cpp **and**
stable-diffusion.cpp adapters linked (one GGUF each), embedded web UI (`HCS_HAVE_GUI`) enabled.
Vulkan image paths are NOT RUN in this evidence set (see `docs/test-evidence/M2/image` for the earlier Vulkan runs).

Real models used:

| Role | File | Notes |
|---|---|---|
| Image (diffusion) | `D:\HCSModels\diffusion\sd15-q8_0.gguf` | SD1.5 Q8_0, 1.76 GB; licence not verified |
| Text (for the UI/API phase) | `E:\AILOCALTRAIN\data\gguf\qwen3-0.6b-q4_k_m.gguf` | Q4_K_M |
| Vision (for the UI phase) | `D:\HCSModels\vision\SmolVLM2-500M-Video-Instruct-Q8_0.gguf` + `mmproj-...Q8_0.gguf` | Q8_0 |

## Evidence files

| File | Content |
|---|---|
| `e2e-image-cpu.log` | stdout of `tests/e2e/image_real/e2e_image.py` against the real binary: **47 checks, 0 failed** |
| `e2e-image.json` | the same run as JSON (`result_status: PASS`), one entry per check with detail on failure |
| `http-generate-256x256-2steps.png`, `http-edit-256x256-2steps.png` | the two images the HTTP phase produced (256x256, 2 steps, SD1.5) |
| `mcp-generate-256x256-2steps.png`, `mcp-edit-256x256-2steps.png` | the two images returned through MCP `hcs.image.generate` / `hcs.image.edit` as image content blocks |
| `../M10-fixes/ctest-full-400-green.txt` | the full CTest suite (400 tests) that contains `e2e_image_real` |

Commands (from `D:\HCSInfer`, MSVC environment via `scripts\common.ps1` `Initialize-MsvcEnvironment`):

```
cmake --preset windows-msvc-release -B build/img-api
cmake --build build/img-api --parallel 4
set HCS_TEST_GGUF=E:\AILOCALTRAIN\data\gguf\qwen3-0.6b-q4_k_m.gguf
set HCS_TEST_VLM=D:\HCSModels\vision\SmolVLM2-500M-Video-Instruct-Q8_0.gguf
set HCS_TEST_MMPROJ=D:\HCSModels\vision\mmproj-SmolVLM2-500M-Video-Instruct-Q8_0.gguf
set HCS_TEST_DIFFUSION_MODEL=D:\HCSModels\diffusion\sd15-q8_0.gguf
ctest --test-dir build/img-api --output-on-failure --timeout 2400
```

## What the 47 checks prove (summary)

HTTP phase (`HCSInfer serve` + real `curl.exe` client):

* the embedded UI is served on the same listener (`GET /` -> 200 html, strict CSP, no token in the page,
  `/ui/app.js` + `/ui/app.css`; DNS-rebinding `Host` and foreign `Origin` -> 403, `POST /` -> 405, HEAD -> 200);
  the API routes still demand the bearer token next to the UI;
* the registry lists the diffusion model with `kind=image`, `/hcs/v1/runtime` reports an available image engine;
* validation before rendering: size not a multiple of 8 -> 400, steps over the cap -> 400, oversized image -> 400,
  no model loaded and none requested -> 409, missing token -> 401;
* a real render: `POST /v1/images/generations` -> 200 with a PNG of the requested size that is **not blank**
  (pixel variety checked), plus `job_id`, `steps`, timing, and a `GET /hcs/v1/jobs/{id}` result summary;
* a real edit: `POST /v1/images/edits` -> 200, PNG of the **input** size, differs from the input; a non-image -> 400;
  a `file:` URL -> 400 (the server never reads a path named by a request);
* `Content-Type: multipart/form-data` -> **415** with a hint to send JSON. (The previous evidence run recorded `400`
  for this one check. The current binary answers `415`, confirmed independently with `curl.exe` against
  `build/img-api/bin/HCSInfer.exe` and by this run. The cause of the earlier `400` was not established; the test
  script itself was also found broken in the working tree - a multi-line string had been corrupted into a Python
  syntax error - and was fixed before this run.)
* a client that gives up mid-render (curl timeout) does not wedge the engine - the next request completes promptly
  because the abandoned job is cancelled (M10 audit D3);
* model unload at the end.

MCP phase (real `HCSInfer mcp stdio` child process, scripted JSON-RPC client):

* `tools/list` includes the image tools; `hcs.models.list` finds the diffusion model;
* `hcs.image.generate` with a bad size -> `invalid_argument` before the engine;
* `hcs.image.generate` (real) succeeds and returns an MCP **image content block** (base64 PNG of the requested size,
  not blank) with `structuredContent` carrying metadata only (no pixels duplicated);
* `hcs.image.edit` (real) succeeds, keeps the input size; an input outside the approved roots or containing `..`
  -> `security_violation` without reading anything;
* cancelling a long render lets the next render complete promptly.

## NOT RUN / limitations

* Vulkan image paths in this evidence set (CPU-only build here); the Vulkan CLI runs are in `../M2/image`.
* Only SD1.5 Q8_0 was exercised; other diffusion families (Flux/DiT/Qwen-Image) and FP8/INT8 paths are untested.
* One image size (256x256) and 2 steps: this is a correctness/route smoke test, not a performance benchmark.
* The image e2e takes ~86 s wall on this machine (a real 2-step render, plus the deliberately abandoned long render).
