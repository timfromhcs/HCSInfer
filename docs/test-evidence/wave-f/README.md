# Wave F: FLUX.3 Image Klein rendered through the HCS adapter

Date: 2026-11-20. Machine: Windows 11 26200, Ryzen 7 7735HS, Radeon 680M (`Vulkan0`, AMD proprietary driver, UMA,
fp16, warp size 32, no matrix cores), ~10.7 GB free RAM at start. Build: preset `windows-msvc-release-vulkan`
(`SD_USE_UPSTREAM_GGML=ON`, Ninja, MSVC 19.44) configured from a `subst` drive (`Y:`), 782 targets, EXIT 0.
Submodules at the recorded pins: stable-diffusion.cpp `532d2c15c430a785a712b2d94281f04e733008a4`, llama.cpp (shared
GGML) `d3c3e242c16b8cbf51400fad410cd69b22d64f35`. No pin was changed.

## Root cause of the earlier BLOCKED state

Not a code or upstream defect. The gated `black-forest-labs/flux-3-image-klein` encoder/VAE could not be downloaded
(see `../flux3/flux3-bfl-denied.txt`). With the user-approved substitutes the pinned stable-diffusion.cpp renders
FLUX.3 Klein unchanged: Qwen3-VL-4B-Instruct Q4_K_M as `--llm` and the FLUX.2 VAE as `--vae`.

| File | Source | Bytes | sha256 (`download-sha256.txt`) |
|---|---|---|---|
| `Qwen3VL-4B-Instruct-Q4_K_M.gguf` | Qwen3-VL-4B-Instruct GGUF | 2,497,281,664 | `66358cb18bb6b3b1b6675aa412c7a88ef01d228f481184d13668e5201c730a0a` |
| `flux2-vae.safetensors` | FLUX.2 VAE (`split_files/vae`) | 336,213,556 | `d64f3a68e1cc4f9f4e29b6e0da38a0204fe9a49f2d4053f0ec1fa1ca02f9c4b5` |

Both hashes equal the content hash embedded in the Hugging Face download cache file names. The DiT is the unsloth
`flux-3-image-klein-UD-Q4_K_XL.gguf` (sha256 `0a057e07...e9fa7`, see `../flux3/README.md`).

## Real renders (Vulkan compute, params on CPU, 512x512, seed 42)

* `try-vk-4.png`: first render, 4 steps, engine default cfg. Rendered, but over-saturated with artefacts.
* Finding and fix: the engine-wide default cfg is wrong for the guidance-distilled Klein. `sweep.py` passes
  `--cfg 1.0` and its 4-step image is clean. `DiffusionImageEngine` now treats an unset cfg as 1.0 for the FLUX.3
  family. `default-cfg-fixed-vk-4.png` (no `--cfg`) matches the sweep 4-step image at PSNR 61.6 dB
  (run-to-run Vulkan numeric noise) versus 13.8 dB before the fix (`try-vk-4.png`). Both numbers were recomputed in wave A
  from the saved PNGs with ImageMagick: `../wave-a/psnr-cfg-fix.txt` (61.6073 dB and 13.7692 dB).
* Step sweep (`sweep-vulkan/`, `sweep.json`, `sweep-montage.png`; ImageMagick 7.1.2; one sample per point, so
  timings are provisional per ADR-010; prompt in `sweep.py`):

| Steps | Wall time | PSNR vs 16 steps | DSSIM vs 16 steps (0 = identical) |
|---|---|---|---|
| 16 | 470.7 s | reference | reference |
| 12 | 431.6 s | 30.0 dB | 0.024 |
| 8 | 399.2 s | 24.1 dB | 0.077 |
| 6 | 364.0 s | 24.1 dB | 0.091 |
| 4 | 370.7 s | 20.3 dB | 0.147 |

Wall time is dominated by a fixed ~300 s per process (Qwen3-VL encoder + model setup); each sampling step costs about
10 s. Distilled models change composition with the step count, so PSNR against the 16-step image is a coarse proxy,
not a quality score; LPIPS was not run (no reviewed neural stack on this machine).

* Decision: `fast` = 6 steps, cfg 1.0 (PSNR equal to 8 steps, 23 % less wall time than 16; 4 steps falls to 20 dB
  and is not faster in this single sample). `kFlux3Fast` changed from the provisional 8.
* Image-to-image (`edit`, `--strength 0.6 --preset fast`, input = the 6-step render): PASS,
  `img2img-vk-fast.png` / `.json` / `img2img-montage.png` (374 s).
* CPU compute path: exercised by the integration test `DiffusionPreset.Flux3PresetsReplaceStepsAndReportThem`
  (`ctest-diffusion-engine.log`, Backend::Cpu, 256x256, quality then fast preset, 326 s): PASS.

## Tests

* `hcs_test_diffusion_engine` with `HCS_TEST_FLUX3_MODEL` (sidecar `*.components.json` next to the model) and
  `HCS_TEST_DIFFUSION_MODEL=sd15-q8_0.gguf`: 14 passed, 0 skipped (`ctest-diffusion-engine.log`). The FLUX test now
  also asserts `fast` reports 6 steps.
* Full suite on the Vulkan build (`ctest-full-vulkan.log`, 788 tests, `Flux3Presets` excluded because it was run above): 787 passed
  or skipped, 1 FAILED: `e2e_gui_real`. Its HTTP phase passes (18 checks); the headless-browser boot check returned an
  empty DOM (`(0, '', '')`) and the Playwright phase is NOT RUN (module not installed). This is an environment
  limitation of this worktree session, unrelated to the diffusion change (no GUI asset was changed after the revert), and is
  not hidden or weakened. Real-model llama/vision/API-MCP e2e tests that need other `HCS_TEST_*` variables were
  skipped (NOT RUN).
* SD1.5 regression is part of the same binary (SD1.5 tests ran against the real model).

## Not done / limits

* `params-backend disk` on FLUX, LPIPS, multiple repeats per step count, and a Vulkan-vs-CPU speed comparison at the
  same size: NOT RUN.
* GUI generic step presets (Draft 4 / Fast 8 / Standard 16) are model-agnostic and unchanged.
* FLUX Kommunity License applies to the weights; nothing here redistributes them.
