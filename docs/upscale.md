# 4K upscaler (`upscale: "4k"`)

Status: **Experimental, opt-in, default OFF**. The tiling/blending/resampling pipeline, the PNG glue, the dynamically loaded
ONNX Runtime backend and the integration into image generation (API, MCP, CLI, UI) are implemented and tested. A run with a
**real ONNX super-resolution model is NOT RUN**: there is no upscaler `.onnx` on this machine and nothing is downloaded by
HCSInfer. Timings, RAM and quality (PSNR/SSIM/LPIPS) of a real model are therefore unmeasured. No `onnxruntime.dll` is shipped
or required; the backend was verified against an ONNX Runtime found elsewhere on this machine (below).

## What it does

`src/engines/upscale`, interface `IUpscaleEngine` (`include/hcs/upscale.hpp`): "run one float RGB tile through a super-resolution
model". Everything else is pure CPU code that does not depend on ONNX Runtime:

1. **Plan** from the memory budget: the largest tile of 512/384/256/192/128 px whose estimated peak (source + float
   accumulator and weight plane at model resolution + tile buffers + engine working set, then the Lanczos stage) fits the
   live budget (`ControlPlane` budget `remaining()`; 1 GiB assumed when unknown). A model with a fixed input size uses it
   and fails with `out_of_memory` when it does not fit. Overlap 16 px (at most tile/4).
2. **Tiled 4x pass** (or the model's native scale): overlapping tiles, linear feather blend in the overlaps (seams vanish; a
   synthetic test proves exact reconstruction for a linear model), cancellation between tiles, progress reported per tile.
3. **Lanczos-3 resample** to exactly 3840 px on the long edge, aspect preserved (support scales with the ratio, so no
   aliasing when the model output is larger than the target).
4. **Never upscales beyond the cap**: a source whose long edge is already >= 3840 is returned unchanged
   (`hcs.upscale.status = "skipped"`). Source images are limited to 16 Mpixel before anything is allocated.

## ONNX Runtime backend (no compile-time dependency)

`onnxruntime.dll` is found only in: `<approved root>/upscale/` and the folder of the executable (nothing on `PATH`), and loaded
with `LoadLibrary` at first use. The C API is used through its function-pointer table (version-checked, with a status
round-trip self-check), so the build needs no ORT header or library and a binary without ORT runs normally.
Execution provider: DirectML when the runtime exports it and a session can be created, otherwise CPU; the provider that is
used is reported (`hcs.upscale.provider`), a fallback is never silent. Model: the first `*.onnx` (sorted) in `<root>/upscale/`;
float32, NCHW `[1,3,H,W]` -> `[1,3,s*H,s*W]`.

`OrtRealRuntime.*` run the backend against a **real** `onnxruntime.dll` and a **synthetic** nearest-neighbour ONNX model
(single `Resize` node, made by `tests/unit/upscale/gen_nn_onnx.py`); they read `HCS_TEST_ORT_DIR` / `HCS_TEST_ONNX_DIR` and are
skipped (NOT RUN) when those are unset, which is the default. Run on 2026-11-26 against the ONNX Runtime 1.31.1 CPU
package already present on this machine (`D:\LocalAIBACKEND\.venv`, read-only use, not copied into HCSInfer): 27/27 passed,
including the whole tiled pipeline to 3840 px, a fixed-input-size model, and "explicit DirectML without the provider is an error
and `auto` falls back to CPU and says so" (this ORT build has no DirectML EP, so the DirectML path itself is NOT RUN). Evidence:
`docs/test-evidence/wave-b/b3/ort_real_runtime.txt`. **A real super-resolution model (e.g. Real-ESRGAN) has not been run**:
speed, peak RAM and quality of an actual 4x model are unmeasured.

## Using it

| Surface | How |
|---|---|
| HTTP | `POST /v1/images/generations` with `"upscale":"4k"`; response `hcs.upscale = {status, from_width, from_height, ms, provider}` and `hcs.width/height` = 3840 side |
| MCP | `upscale: "4k"` on `hcs.image.generate`/`hcs.image.edit`; result `structuredContent.upscale` |
| CLI | `HCSInfer image generate ... --upscale 4k [--upscale-dir D:\HCSModels\upscale]` |
| UI | Image Studio, "Upscale" select |

Errors: missing runtime or model -> `unsupported` (HTTP 501, CLI exit 10) naming what is missing, returned **before** the
render starts (a long generation is never wasted). Cancelling during the upscale cancels the job. If the upscale itself fails
after a successful render, the rendered image is returned and `status = "failed"` with the reason (a render is not discarded
because of an optional post-step).

## Tests

`tests/unit/upscale` (tiling plan, feather weights, seam test, budget-bounded plan, Lanczos against an independent NumPy
implementation (`gen_resample_golden.py`), PNG round trip, cancellation, error mapping), `tests/contract/api_image/test_image_lora_upscale.cpp` (control-plane
wiring with a nearest-neighbour **test double**, used only in tests), MCP and CLI help/contract tests. See
`docs/test-evidence/wave-b/b3/`.
