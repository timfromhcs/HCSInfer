# M2 vision (libmtmd image understanding) - evidence

Date: 2026-11-17. Scope: `LlamaTextEngine::analyze_image` (llama.cpp libmtmd), `HCSInfer vision`, OpenAI-style
`image_url` data URIs on `POST /v1/chat/completions`, MCP `hcs.vision.analyze`.

## Result summary

| Gate | Status | Evidence |
|---|---|---|
| libmtmd linked from the pinned llama.cpp (no upstream edits), CPU build | PASS | `build/vision` (windows-msvc-release preset, `-B build/vision`), llama.cpp `d3c3e242c16b8cbf51400fad410cd69b22d64f35` |
| libmtmd linked, Vulkan build | PASS | `build/vision-vk` (windows-msvc-release-vulkan preset), AMD Radeon(TM) Graphics (`Vulkan0`); mtmd log `mtmd_backends: using Vulkan0 backend`, `offloaded 33/33 layers to GPU` (`cli-vision-vulkan-backend-log.txt`) |
| Real VLM image analysis, engine level (CPU) | PASS | `gtest-llama_vision-cpu-build.log`: 9 PASS, 1 NOT RUN (the Vulkan test: that build has no GPU backend) |
| Real VLM image analysis, engine level (Vulkan) | PASS | `gtest-llama_vision-vulkan-build.log`: 10/10 PASS incl. `DescribesAGeneratedImageOnVulkan` |
| CLI `HCSInfer vision`, CPU and Vulkan | PASS | `cli-vision-cpu.json`, `cli-vision-vulkan.json` |
| CLI error paths (no mmproj, mismatched mmproj, mmproj as main model, missing/invalid/`..` image, explicit Vulkan on CPU-only build) | PASS (clean exit codes 2/3/10/7/2/9/10, no crash) | `cli-vision-negative.txt` |
| HTTP image input (non-stream, SSE, rejects, 413, 401, client-disconnect cancel), real model, CPU and Vulkan builds | PASS 27/27 each | `e2e-vision-cpu.{log,json}`, `e2e-vision-vulkan.{log,json}` |
| MCP `hcs.vision.analyze` real client over stdio, CPU and Vulkan builds | PASS (inside the same 27 checks) | same files |
| Unit + contract tests (generated PNG, fake vision engine) | PASS | `tests/unit/api_vision` (7), `tests/contract/api_vision` (14); full ctest `ctest-all-no-models.txt`: 100% passed out of 327, real-model tests reported as skipped = NOT RUN when no model env var is set |

Not covered / NOT RUN: any VLM other than SmolVLM2-500M-Video-Instruct Q8_0 (other projector families such as
Qwen-VL M-RoPE, Gemma3 non-causal attention are untested); multi-image and multi-turn image chat (rejected with a
clear 400); video/audio input (`MTMD_VIDEO=OFF`, rejected); GUI; memory-budget accounting of the projector; HDD-loaded
(USB) cold-load measurements for the VLM (the VLM lives on `D:`); a controlled benchmark (the single timings below are
provisional smoke numbers: one sample, uncontrolled thermals, first-run Vulkan shader compile included).

## Test assets and models

* Test image: generated in-process / by the e2e script (`tests/support/test_png.hpp`, `e2e_vision.py`): a 256x256
  solid red square (128x128 px) on white, saved here as `red-square-256.png` (generated, no third-party content,
  freely redistributable).
* VLM: `D:\HCSModels\vision\SmolVLM2-500M-Video-Instruct-Q8_0.gguf`, 436,808,704 bytes, sha256
  `6f67b8036b2469fcd71728702720c6b51aebd759b78137a8120733b4d66438bc`
* mmproj: `D:\HCSModels\vision\mmproj-SmolVLM2-500M-Video-Instruct-Q8_0.gguf`, 108,785,184 bytes, sha256
  `921dc7e259f308e5b027111fa185efcbf33db13f6e35749ddf7f5cdb60ef520b`
* Model licence: SmolVLM2 is published under Apache-2.0 by Hugging Face; **not re-verified in this session** and the
  download source was not recorded (open item, same as the earlier M1 note).
* Mismatch/negative text model: `E:\AILOCALTRAIN\data\gguf\qwen3-0.6b-q4_k_m.gguf` (sha256 `4ea79412...` per M1).
* Machine: Windows 11 Pro 10.0.26200, MSVC 19.44.35207, Ninja, Radeon 680M (reported by ggml as "AMD Radeon(TM)
  Graphics", 13515 MiB free on Vulkan0). The AMD driver/Vulkan API version was not captured by this run (`unknown`).

## Commands (exact) and results

Builds (own build dirs, `--parallel 4` / `3`):

```powershell
. scripts\common.ps1; Initialize-MsvcEnvironment
cmake --preset windows-msvc-release        -B build/vision
cmake --build build/vision --parallel 4
cmake --preset windows-msvc-release-vulkan -B build/vision-vk
cmake --build build/vision-vk --parallel 3 --target hcsinfer hcs_test_llama_vision
```

Real-model engine tests (CPU build; env: `HCS_TEST_VLM`, `HCS_TEST_MMPROJ`, `HCS_TEST_GGUF`):

```powershell
build\vision\bin\hcs_test_llama_vision.exe        # 9 PASS + 1 NOT RUN (Vulkan test: no GPU backend in this build)
build\vision-vk\bin\hcs_test_llama_vision.exe     # 10/10 PASS
```

Tests (all in `LlamaVisionReal.*`): describes the generated image (CPU/Vulkan; the answer is " Red."), greedy output is
deterministic across two requests and a text-only job still works on the same session, unload/reload releases the
projector, garbage / header-only 6000x4000 and 60000x60000 / truncated PNG / marker-injection prompts fail with
`InvalidArgument` or a failed job and the engine stays usable, cancellation mid-answer ends `Cancelled` and the context
is reusable, text-only load then `analyze_image` is a clean `Unsupported`, missing / non-GGUF / truncated mmproj are
clean load errors that leave nothing loaded, a Qwen3 text model with the SmolVLM mmproj is rejected
(`model_error: mismatch between text model (n_embd = 1024) and mmproj (n_embd = 960)`), and an mmproj passed as the
main model is `unsupported` ("CLIP cannot be used as main model").

CLI:

```powershell
build\vision\bin\HCSInfer.exe vision --model D:\HCSModels\vision\SmolVLM2-500M-Video-Instruct-Q8_0.gguf `
  --image red-square-256.png --prompt "What colour is the large square in this image? Answer in one short sentence." `
  --temp 0 --max-tokens 40 --backend cpu --json
build\vision-vk\bin\HCSInfer.exe vision ... --backend vulkan --json
```

Both answer " Red." (160 prompt tokens, 1 decoded token). Provisional numbers (single sample): CPU load 232 ms,
TTFT 1881 ms (image encode ~924 ms); Vulkan load 932 ms, TTFT 3016 ms on the first run (includes shader/pipeline
creation), image encode 247 ms on a warm second run.

End-to-end over HTTP and MCP (spawns `HCSInfer serve` and `HCSInfer mcp stdio`, real model, stdlib Python):

```powershell
$env:HCS_TEST_VLM = 'D:\HCSModels\vision\SmolVLM2-500M-Video-Instruct-Q8_0.gguf'
python -I tests\e2e\vision_real\e2e_vision.py --exe build\vision\bin\HCSInfer.exe     --evidence ...   # 27 checks, 0 failed (runtime reports backend=cpu)
python -I tests\e2e\vision_real\e2e_vision.py --exe build\vision-vk\bin\HCSInfer.exe  --evidence ...   # 27 checks, 0 failed (runtime reports "AMD Radeon(TM) Graphics (Vulkan0)")
```

Checks include: `image_url` data URI answer mentions red, `usage.prompt_tokens` = 160 (image consumed), deterministic
across requests, SSE text equals non-stream text, `[DONE]`, 400 for remote https / `file://` / bare path / bad base64 /
non-image media type / bytes that are not an image / 100000x100000 declared dimensions / two images, 413 for an
oversize body, 401 without a token, client disconnect during a streamed 800-token request leaves no job pending within
10 s (so the vision job was cancelled), engine healthy afterwards; MCP: answer mentions red, path outside the approved
roots and `..` traversal are `security_violation`, a non-image file is `invalid_argument`, server still responsive.

## Findings fixed during this work (root causes)

1. `LNK2005` for 43 `stbi_*` symbols: libmtmd compiles its own `vendor/stb` with external linkage and
   `hcs_engine_diffusion` compiles stable-diffusion.cpp's copy. Fixed without touching upstream: the 48 public stb
   symbols are renamed (`hcs_mtmd_` prefix) for the `mtmd` target only (`src/engines/llama/CMakeLists.txt`).
2. libmtmd has its own log hook; without `mtmd_helper_log_set` its errors went to stderr and not into the adapter's
   error text. Now captured, so a projector mismatch reports the upstream reason in the error message.
3. stb's header scan rejects a PNG that has only an IHDR chunk, so the engine's `get_media_meta` precheck reports "not a
   supported image or dimensions out of range" for such input; the explicit "Mpixel" message is produced for
   structurally valid headers (and by the HTTP layer's own header probe before any decoder runs).

## Notes / open issues

* `ControlPlane` pairs a model with its projector by folder convention (`projector_path_for`): the `mmproj*.gguf`
  whose name minus quantisation/`mmproj` tokens equals the model's, else the only projector next to the only other
  model. Ambiguous folders are not guessed (the model loads text-only; image requests get a clean `unsupported`).
  The registry still lists mmproj files as ordinary `text` models (`architecture: clip`); loading one as a model fails
  with `unsupported`.
* To send photos over HTTP raise `serve --max-body-bytes` (default 1 MiB limits a base64 image to ~750 KB).
* `analyze_image` accepts a single image and no history: single-turn semantics like text chat.
* `HCS_TEST_EVIDENCE_DIR` results are appended; `llama_vision-cpu-build.jsonl` therefore contains lines from several runs.
