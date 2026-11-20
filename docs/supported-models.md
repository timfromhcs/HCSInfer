# Supported models

Only models that were loaded and run through HCSInfer on the target machine are listed as `Verified`. Everything
else is `Recognised` (the engine identifies it) or `Blocked` (a required file could not be obtained), with the
reason. Model files live outside the repository (the evidence uses `D:\HCSModels`).

## Text and vision (llama.cpp adapter)

| Model | Files | Status | Evidence |
|---|---|---|---|
| **LiquidAI LFM3-Omni-2B Q8_0** (default local LLM) | `LFM3-Omni-2B-Q8_0.gguf` (1.85 GB) + `mmproj-LFM3-Omni-2B-Q8_0.gguf` (0.51 GB) in one folder | Verified: chat, streaming, cancellation, **tool calling**, image understanding (CPU + Vulkan); audio / video input and speech output NOT RUN (not wired) | `docs/test-evidence/lfm3` |
| Qwen3-0.6B Q4_K_M | one GGUF | Verified (text, CPU + Vulkan; B0/B1 benchmark) | `docs/test-evidence/M1/llama`, `benchmark` |
| SmolVLM2-500M-Video-Instruct Q8_0 | GGUF + mmproj | Verified (image understanding, CPU + Vulkan) | `docs/test-evidence/M2-vision` |

### LFM3-Omni-2B (default LLM)

* Architecture `lfm3`; supported by the pinned llama.cpp `d3c3e24` for text and the omni projector (`LFM3VL`), so **no pin
  change** was needed. Native context 128k; HCSInfer loads it with its 4096-token default (the KV cache of the native context
  does not fit). Recommended sampling from the model card: greedy for text.
* Measured on this PC (`HCSInfer benchmark --suite quick`, warm medians, one controlled run each): Vulkan 35.9 decode tokens/s, TTFT 438 ms; CPU
  18.8 decode tokens/s, TTFT 2,790 ms (`docs/test-evidence/lfm3/README.md`, with caveats).
* HCSInfer adds the BOS token and the empty `<think></think>` block that the model's chat template uses (the generic ChatML path
  lacks them; without the BOS the model drifts into `<|audio_start|>` output).
* **Tool calling** (`POST /v1/chat/completions` with `tools`): the XML tool format of the model's chat template is used, results
  come back as OpenAI `tool_calls` (non-stream and SSE). `tool_choice` `auto`/`none` only; `required`/named function and
  legacy `functions` return 501. The model emits one call per turn (parallel calls: 0/3 on the held-out eval).
* Held-out agentic eval baseline (`tests/eval/agentic`, 40 tasks): **37/40 = 0.925** (tool use 0.906, coding 1.000).
* Licence: **LFM Open License v1.0** - commercial use only for entities below US$ 10M annual revenue (section 5). Weights are never
  redistributed by HCSInfer; sha256 `59118f66...158d4` (Q8_0) and `8a25f9d1...c27b1` (mmproj), full values in the evidence README.

## Image generation (stable-diffusion.cpp adapter)

| Model | Files | Status | Evidence |
|---|---|---|---|
| Stable Diffusion 1.5 Q8_0 | one GGUF | Verified (txt2img, img2img, CPU + Vulkan) | `docs/test-evidence/M1/sd`, `M3-image`, `B7-params` |
| FLUX.3 Image Klein (5.6B distilled), unsloth `UD-Q4_K_XL` | DiT GGUF + Qwen3-VL-4B-Instruct text encoder + FLUX.2 VAE | Rendered (Vulkan and CPU, text-to-image and image-to-image) | `docs/test-evidence/wave-f` |

### FLUX.3 Image Klein

* The pinned stable-diffusion.cpp (`532d2c1`) already contains the FLUX.3 pipeline; no pin change was made. Under
  `SD_USE_UPSTREAM_GGML=ON` the unsloth `flux-3-image-klein-UD-Q4_K_XL.gguf` (3,929,944,992 bytes, sha256
  `0a057e07550a5b2169e344d686171a7dd3c7d82c7d8d1d1605d8d881c16e9fa7`, verified against the Hugging Face LFS hash)
  is identified as `Flux.3 Image Klein` and its DiT (hidden 3072, 5 double + 28 single blocks) is parsed.
* It is a multi-file model. The DiT GGUF contains no text encoder and no VAE; both are separate files:
  the encoder is Qwen3-VL-4B-Instruct (`--llm`), the VAE is the FLUX.2 VAE (`--vae`).
  **Status: the gated `black-forest-labs/flux-3-image-klein` files are not accessible for the configured account.
  With Qwen3-VL-4B-Instruct Q4_K_M and the FLUX.2 VAE (hashes in `docs/test-evidence/wave-f`) a 512x512 render takes
  about 364-471 s on Radeon 680M (Vulkan, 6-16 steps; ~300 s of that is a fixed per-process cost), single samples.**
  `--preset fast` = 6 steps, cfg 1.0; an unset cfg means 1.0 for this family.
* Licence: FLUX Kommunity License v1.0 (Black Forest Labs; non-commercial / non-production, or commercial only as
  a Qualifying User). Weights are never redistributed by HCSInfer.
* No public "fast" LoRA for FLUX.3 Klein was found or downloaded. The adapter does not claim one.

#### Loading FLUX.3 through HCSInfer

Explicit components:

```
HCSInfer image generate --model <flux.gguf> --llm <qwen3-vl-4b.gguf-or-safetensors> --vae <flux2-vae.safetensors> \
    --prompt "..." --preset quality
```

or a sidecar next to the model, `<model>.components.json` (relative paths only, no `..`, must stay under the model
directory; read as untrusted input):

```json
{"llm": "encoders/qwen3-vl-4b.gguf", "vae": "vae/flux2_ae.safetensors", "params_backend": "cpu"}
```

`--params-backend cpu|disk` (also `LoadRequest::params_backend`) selects where module weights live between uses.
`disk` is a capacity fallback and can be slower; on SD1.5 it was both smaller and faster only because the model
already sat on a fast drive (see `docs/test-evidence/B7-params`).

#### Presets

`--preset fast|quality` (`ImageRequest::preset`, HTTP `preset`, MCP `preset`) replaces `steps` and `cfg_scale`
with the point for the loaded model family. Only the `flux3` family has a table:

| preset | steps | cfg | basis |
|---|---|---|---|
| `quality` | 16 | 1.0 | the upstream stable-diffusion.cpp FLUX.3 reference (`docs/models/flux3.md`: 16 steps, CFG 1.0) |
| `fast` | 6 | 1.0 | **Measured (wave F, single sample per point, ADR-010 provisional).** The step sweep 4/6/8/12/16 on the Radeon 680M picked 6 steps as the quality/time point; PSNR/DSSIM vs the 16-step reference and timings are in `docs/test-evidence/wave-f/` (LPIPS NOT RUN). |

A preset on a model family without a table (for example SD1.5) is rejected with `InvalidArgument`; it is never
silently ignored. The applied preset and the effective step count are returned in the result.
