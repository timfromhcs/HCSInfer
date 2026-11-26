# Wave B b3 evidence: image LoRA library, 4K upscaler pipeline, model merge

Date 2026-11-26. Branch `wave-b/b3`. Build: `windows-msvc-release` (CPU), `D:\HCSInfer\build\b3`, `-j4`, MSVC 19.44.35228,
`/W4 /WX`. clang-format 19.1.5 check: OK (255 files). No downloads, nothing written to drive C: (all caches/temp under
`D:\HCSScratch`).

| File | What it proves |
|---|---|
| `ctest-full-cpu.txt` | one full serial CTest run after the final build: 928 registered, 0 failed, 41 skipped. The skipped ones are real-model / environment-dependent tests (no `HCS_TEST_*` variables set on purpose, so no real model ran in this run) = NOT RUN here |
| `build-tail.txt` | last build log (exit 0) |
| `ort_real_runtime.txt` | `hcs_test_upscale` with `HCS_TEST_ORT_DIR` = the ONNX Runtime 1.31.1 CPU package already on this machine and a synthetic nearest-neighbour ONNX model: 27/27 passed incl. `OrtRealRuntime.*` (onnxruntime.dll SHA-256 recorded in the file) |
| `real_merge.txt` | `HCSInfer merge` linear and slerp with the real `flux2-vae.safetensors` (320.6 MB, 251 F32 tensors) as base and model: exit 0, ~6 s, peak working set 46 MB, output SHA-256 |
| `real_merge_verify.txt` | NumPy check: every tensor of both outputs equals the input (max abs diff 0.0) |

## Test inventory added or used by this work

* `tests/unit/lora` (11), `tests/unit/core/test_safetensors_probe.cpp`: manifest, scanner, family compatibility, weight bounds, resolve.
* `tests/contract/api_image/test_image_lora_upscale.cpp` (13): `/hcs/v1/loras`, ids-only resolution, weights, family mismatch,
  missing library, upscale off/4k/missing runtime/invalid value/tile failure.
* `tests/contract/mcp_image` (+2) and `tests/contract/mcp` (tool list): schemas, `hcs.loras.list`, unsupported paths.
* `tests/contract/cli/test_cli_merge.cpp` (3), `tests/unit/gui/test_gui_assets.cpp` (LoRA picker / upscale wiring).
* `tests/unit/upscale` (27), `tests/unit/merge` (25) with NumPy goldens (`gen_*_golden.py`, generated in a venv under `D:\HCSScratch`).

## NOT RUN / open

* Real LoRA render (no LoRA file); real 4x super-resolution model run, timings, RAM, PSNR/SSIM/LPIPS (no model, ORT DirectML EP absent);
  a merge of two distinct real models and loading a merged model into an engine; merge via HTTP/MCP; CLI `image --lora/--upscale`
  end to end with a real diffusion model (only argument/validation paths and the control-plane plumbing with fakes are tested).
* The `D:\HCSModels\lora\image\` and `D:\HCSModels\upscale\` folders were created with a README each; they hold no model files.
