# M1 stable-diffusion.cpp independent baseline

Date 2026-11-17. Host: Ryzen 7 7735HS, Radeon 680M (Vulkan0, AMD proprietary driver), Windows 11, MSVC 19.44, Ninja, CMake 4.4, Vulkan SDK 1.4.357.0.
Pins: sd.cpp 532d2c15c430a785a712b2d94281f04e733008a4; nested patched ggml eeda9ec48fc84a3df0bb9dcc19ae9053075a2539; llama.cpp (upstream ggml source) d3c3e242c16b8cbf51400fad410cd69b22d64f35. Submodules initialised: ggml, libwebp, libwebm (shallow). frontend/cutlass not needed.

## Gates
| Gate | Status |
|---|---|
| sd.cpp Vulkan build, patched GGML (build/sd-vulkan) | PASS (sd-vulkan-configure.log, sd-vulkan-build.log) |
| sd.cpp build, SD_USE_UPSTREAM_GGML=ON (build/sd-upstream-ggml) | PASS (configure+build exit 0; only the documented limitation warning) |
| Device listing (both builds) | PASS: Vulkan0 AMD Radeon(TM) Graphics + CPU |
| Real diffusion smoke 256x256, 4 steps, Vulkan | PASS (pipeline ran, image saved; 256px/4-step/cfg1 output is noise-like, not semantically meaningful) |
| Real diffusion 512x512, 15 steps, cfg 7, Vulkan | PASS, visually a red apple; patched == upstream output byte-identical |
| CPU-only smoke 256x256, 4 steps (--backend cpu) | PASS, 87.0 s generate_image |
| Real Radeon 680M diffusion test | PASS (SD1.5 only) |
| Other model families (Flux/DiT/Qwen-Image), disk params, FP8/INT8 | NOT RUN |
| Combined llama.cpp+sd.cpp GGML graph / single EXE | NOT RUN |

## Model used
D:\HCSModels\diffusion\sd15-q8_0.gguf (1,763,578,176 bytes, SD1.5 Q8_0 GGUF, sha256 d0555243938c62faeefb4ac93f6c7a053ad373a4290c5256bce229aeb193bf94). It lives on the internal D: NVMe, not the USB HDD; licence/source of this file is unknown (not provenance-verified). Other local diffusion-like files (E:\Qwen\Qwen-Image-2.1-*, E:\qwen-quant-*) are multi-GB diffusers safetensors (Qwen-Image transformer ~13.5 GB + text encoder), not tested: too large for ~19.8 GB RAM with sd.cpp conversion and not a small smoke target. Full search output: model-search.txt.

## Timings (single samples, NOT benchmarks; first runs overlapped a concurrent compile)
- Vulkan 256/4 steps, first run (build running concurrently, cold shader cache): generate_image 45.4 s; uncontended repeat 3.16 s.
- Upstream-GGML Vulkan 256/4: 4.62 s.
- CPU 256/4: 87.0 s.
- Vulkan 512/15 steps: 38.3 s (patched), 38.9 s (upstream GGML).
Model-load note: tensors read ~390 MB/s from internal NVMe-class storage.

## Commands
Build scripts: sdvk.bat, sdup.bat, sdupb.bat (cmake -G Ninja -DSD_VULKAN=ON -DSD_BUILD_EXAMPLES=ON -DSD_BUILD_SHARED_LIBS=OFF -DSD_SERVER_BUILD_FRONTEND=OFF -DCMAKE_BUILD_TYPE=Release [-DSD_USE_UPSTREAM_GGML=ON -DSD_GGML_SOURCE_DIR=D:/HCSInfer/external/llama.cpp/ggml]).
Smoke: `sd-cli.exe -m D:\HCSModels\diffusion\sd15-q8_0.gguf -p "a photo of a red apple on a wooden table" -W 512 -H 512 --steps 15 --cfg-scale 7 --seed 42 --backend vulkan0 --params-backend cpu -o out.png -v`
CPU: same with `--backend cpu`. Logs: smoke-*.log. Images: images/. Help: sd-cli-help.txt. Devices: sd-cli-list-devices.txt, upstream-list-devices.txt.

## Observations / open
- Repeated identical 256px Vulkan runs produced different PNGs once (non-determinism not investigated).
- Storage: params-backend=disk and USB-HDD load not tested (model is not on the HDD; cold/warm not measured). Recommended supported small test if needed elsewhere: this SD1.5 file is sufficient; for the HDD gate copy-by-user to E: is a user decision.
- ADR: docs/decisions/ADR-001-engine-integration.md (draft).
