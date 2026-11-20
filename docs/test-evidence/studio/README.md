# HCS Studio evidence (2026-11-17)

Status: **PASS** for the UI redesign. NOT RUN: python-playwright phase of `e2e_gui.py` (package not installed); Vulkan
(the server used for the real-model run is the CPU build).

| Check | Result | Artefact |
|---|---|---|
| `hcs_test_gui_assets` (12 tests incl. new `StudioStructure`) and `hcs_test_api_image` (17, incl. `ApiUi.*` CSP/Host/Origin/405/401/no-token-embedding) | PASS | `ctest-gui-unit.txt`, `ctest-api-image.txt` |
| `e2e_gui.py` against the freshly built exe (UI embedded in the binary): CSP, traversal, Host/Origin, headless boot connects with fragment token | PASS (22 checks, 1 NOT RUN) | `e2e-gui-embedded/e2e-gui.json` |
| `studio_shots.py` against a real `HCSInfer serve`: 42 checks (no horizontal scroll on 8 tabs at 1360/820/390 px, Planned tabs have no controls, real model load via Models tab, streamed chat, Stop, memory split, text-to-image, image-to-image, no CSP violations, no console errors) | PASS | `studio-shots.json`, screenshots |

Screenshots: `{desktop,phone}-{light,dark}-{chat,image,vision,models,perf,settings,agent,train}.png`, plus
`desktop-light-models-loaded`, `desktop-light-chat-reply`, `desktop-light-image-{working,txt2img,img2img}`, `phone-light-image-result`.

## Setup and caveats (exact)
* Models (read-only, from `<MODELS>`): `SmolVLM2-500M-Video-Instruct-Q8_0.gguf` (chat), `sd15-q8_0.gguf` (diffusion, 256x256, 4 steps, seed 42).
* Server: the existing CPU build `<REPO>\build\img-api\bin\HCSInfer.exe` (`serve --timeout-s 1200 --max-body-bytes 8388608`), with `tests/e2e/gui_real/ui_proxy_dev.py` serving this worktree's `src/gui/assets` in front of it. Reason: `external/llama.cpp` and `external/stable-diffusion.cpp` were being re-populated by another agent during this task (configure failed on missing `vendor`/`thirdparty`), so an engine-linked binary with the new UI could not be built here. The embedded-asset path was verified separately with an engine-less build of this tree (copy of the sources in the session scratchpad, no `external/`).
* SD1.5 is not a step-distilled model: the 4-step images are noisy by design. The screenshots show that the UI works, not image quality. No LoRA or FLUX.3 run is claimed here.
* The first image attempt hit the server's default 300 s request timeout (HTTP 504, shown by the UI as an error); the final run used `--timeout-s 1200`.
* Screenshots come from headless Edge over the DevTools protocol; browser UI strings (file picker) follow the machine locale.
