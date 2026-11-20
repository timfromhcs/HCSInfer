# FLUX.3 Image Klein through the HCS diffusion adapter

> Update 2026-11-20: the render is no longer blocked. Results, the step sweep and the `fast` preset decision are in
> `../wave-f/README.md`. The text below is the 2026-11-17 state and stays as history.

Date: 2026-11-17. Machine: Windows 11 26200, Ryzen 7 7735HS, Radeon 680M (Vulkan0, AMD proprietary driver, UMA),
MSVC 19.44, CMake 4.4, Ninja. Build: `windows-msvc-release-vulkan`, `SD_USE_UPSTREAM_GGML=ON`, built from a short
`subst` drive (`Y:\build\f3`) because the Vulkan shader-generator sub-project exceeds MAX_PATH from the long
worktree path. stable-diffusion.cpp pin `532d2c15c430a785a712b2d94281f04e733008a4` (`master-1106-532d2c1`),
unchanged. The worktree's `external/llama.cpp` (the shared upstream GGML) was at `ad9d9e8f5` when built, a newer
commit than the lock-file pin `d3c3e24`; another workstream moved it, not this change.

## Status per gate

| Gate | Result |
|---|---|
| Pinned sd.cpp supports FLUX.3 Klein | PASS. `VERSION_FLUX3` and `src/model/integrations/flux3.cpp` exist at `532d2c1`; no submodule bump needed. |
| CPU + Vulkan build with `SD_USE_UPSTREAM_GGML=ON` | PASS (`build/f3`, 674 targets, no errors) |
| SD1.5 regression | PASS (below) |
| Approved downloads | PARTIAL: unsloth GGUF PASS (sha256 verified); LFM3 Q8_0 + mmproj downloaded (hash file below); BFL text encoder + VAE BLOCKED |
| FLUX.3 text-to-image + image-to-image on Vulkan; CPU text-to-image (256x256 test) only | **PASS** with the approved substitute encoder/VAE; see `../wave-f/README.md` (2026-11-20) |
| Memory split DiT / Qwen3-VL encoder / VAE, params-backend cpu vs disk on FLUX | **NOT RUN** (render no longer blocked; not measured) |
| Step sweep 4/6/8/12/16 and image diff | **PASS** (PSNR/DSSIM, single sample); LPIPS NOT RUN. `../wave-f/sweep-vulkan/` |
| Fast LoRA | NOT RUN: no public FLUX.3 Klein fast LoRA exists; none was claimed or downloaded |
| Presets in ImageRequest / CLI / HTTP / MCP | PASS (code + tests); `fast` = 6 steps is now measured (wave F) |

## Why the render is blocked

`black-forest-labs/flux-3-image-klein` is gated. With the Hugging Face token already configured on this machine the
repository listing works, but downloading `ae.safetensors` and `text_encoder/*` returned
`Error: Access denied. This repository requires approval.` (`flux3-bfl-denied.txt`). The account has not been
granted access. The unsloth GGUF contains only the transformer ("the transformer weights were quantized to GGUF");
it holds no text encoder and no VAE, so it cannot render alone. Only the approved files were requested; no
substitute encoder or VAE was fetched from another repository.

To unblock, either approve access to the BFL repository for the Hugging Face account, or approve a substitute
(the sd.cpp FLUX.3 guide lists `Qwen/Qwen3-VL-4B-Instruct-GGUF` for the encoder and the FLUX.2 VAE), then run:

```
python docs/test-evidence/flux3/sweep.py --exe <HCSInfer.exe> --model <MODELS>\diffusion\flux3\flux-3-image-klein-UD-Q4_K_XL.gguf ^
    --llm <qwen3-vl-4b> --vae <flux2-vae> --out docs/test-evidence/flux3/sweep-vulkan --backend vulkan
```

Resources to expect (an estimate, not a measurement): DiT 3.93 GB (Q4_K_XL file), Qwen3-VL-4B about 8.9 GB as the
BFL BF16 safetensors (about 2.5 GB if a Q4 GGUF is used), VAE 0.34 GB, on a machine that reported 8.1 GiB free RAM
during the probe. The auto-fit planner in stable-diffusion.cpp placed the DiT on the CPU with params on the host.

## What was proven

1. **Model recognition** (`probe-no-encoder.log`, upstream `sd-cli` with upstream GGML + Vulkan, CPU compute):
   `Version: Flux.3 Image Klein`; `flux3: hidden_size = 3072, context_dim = 20480, depth = 5, single_depth = 28`;
   GGUF v3, 359 tensors; weight types f32 86 / q8_0 12 / q2_K 24 / q3_K 5 / q4_K 51 / q5_K 88 / q6_K 79 / bf16 14.
   The run then fails (as expected) because the conditioner and VAE tensors are absent.
2. **HCS adapter plumbing** (`flux3-plumbing-with-dummy-components.txt`): with `--llm` and `--vae` the adapter hands
   the model to stable-diffusion.cpp as `diffusion_model_path` + `llm_path` + `vae_path`; the log shows
   `Version: Flux.3 Image Klein` and the loader opening each component. The dummy components are invalid files
   on purpose, so the run ends with a `model_error`; this proves routing only, not a render.
3. **Actionable error without components** (`flux3-load-without-components.txt`): loading the DiT-only GGUF as
   an all-in-one checkpoint now explains that it needs `--llm`/`--vae` or a `<model>.components.json` sidecar.
4. **SD1.5 regression, real model `<MODELS>\diffusion\sd15-q8_0.gguf`**, 256x256, 2 steps, seed 42:
   * CPU PNG sha256 is byte-identical to the one produced by the pre-change HCSInfer binary
     (`d2686dda9d76...`, `sd15-baseline-oldbin-cpu.png` vs `sd15-regression-cpu.png`).
   * Vulkan: the new PNG and the pre-change binary's PNG differ only numerically (PSNR 67.2 dB; Vulkan results also
     moved with the newer shared GGML).
   * Vulkan run: load 1816 ms, generation 4088 ms (`sd15-regression-vulkan.json`).
   * Output at 2 steps is a blur; this is a smoke test, image quality is not judged.
5. **Tests** (real SD1.5 model via `HCS_TEST_DIFFUSION_MODEL`):
   `hcs_test_diffusion_engine` 13 passed, 1 skipped (`ctest-diffusion-engine.log`); the skipped one is
   `Flux3PresetsReplaceStepsAndReportThem`, NOT RUN because `HCS_TEST_FLUX3_MODEL` has no usable model.
   Touched targets: `hcs_test_api_image` 17, `hcs_test_mcp_image` 7, `hcs_test_api_http` 12, `hcs_test_mcp` 27,
   `hcs_test_cli` 13, `hcs_test_api` 49 passed (`ctest-touched-targets.log`).
6. **Presets**: a preset on a model without a family table (SD1.5) is rejected with `invalid_argument`
   (`sd15-preset-rejected.txt`); an unknown preset name is rejected even with no model loaded.

## Preset values (honest status)

`quality` = 16 steps, cfg 1.0 (upstream reference). `fast` = 8 steps, cfg 1.0 is **provisional**: it is half the
reference step count, chosen without a measurement because the sweep could not run. It must not be quoted as the
"best quality/time point". Replace `kFlux3Fast` in `src/engines/diffusion/diffusion_engine.cpp` with the value the
sweep selects. The documented diff metric for that sweep is PSNR and SSIM against the 16-step render using
ImageMagick `compare` (LPIPS needs a neural network not available here).

## Downloads and licences

| File | Source | Size | sha256 | Verified |
|---|---|---|---|---|
| `flux-3-image-klein-UD-Q4_K_XL.gguf` | `unsloth/flux-3-image-klein-GGUF` | 3,929,944,992 | `0a057e07550a5b2169e344d686171a7dd3c7d82c7d8d1d1605d8d881c16e9fa7` | yes, equals the Hugging Face LFS hash |
| BFL `ae.safetensors`, `text_encoder/*` | `black-forest-labs/flux-3-image-klein` | n/a | n/a | not downloaded (access denied) |
| `LFM3-Omni-2B-Q8_0.gguf`, `mmproj-LFM3-Omni-2B-Q8_0.gguf` | `LiquidAI/LFM3-Omni-2B-GGUF` | 1,846,240,768 / 506,842,528 | see `lfm3-byproduct-sha256.txt` against `59118f66...` / `8a25f9d1...` | see file |

Files are on `<MODELS>` (`diffusion\flux3`, `llm\LFM3-Omni-2B`); the Hugging Face cache was redirected to
`<MODELS>\.hfcache`, so nothing was cached on the SSD. FLUX.3 Klein is under the FLUX Kommunity License v1.0
(Black Forest Labs; non-commercial/non-production use, or commercial use only as a Qualifying User); the unsloth
card notes the transformer was quantized to GGUF by Unsloth. LFM3-Omni is under the licence file shipped in its
repository (`<MODELS>\llm\LFM3-Omni-2B\LICENSE`).

The LFM3 files were fetched by the same download chain that was started for this task; they are not used by this
change. A second workstream downloaded the same model to `<MODELS>\text\LFM3-Omni-2B`.
