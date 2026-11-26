# Third-party notices (updated 2026-11-18 from the submodule and fetched LICENSE files; SBOM: `scripts/sbom.ps1`)

Dependencies linked into or fetched by the current build. Exact pins and CMake options:
`external/dependency-lock.json`. Licence texts must be reproduced in release packages.

| Component | Version / pin | Licence | Use |
|---|---|---|---|
| HCSUDA (owner| llama.cpp / GGML (Copyright (c) 2023-2026 The ggml authors; `external/llama.cpp/LICENSE`) | commit `d3c3e242c16b8cbf51400fad410cd69b22d64f35` (submodule `external/llama.cpp`) | MIT | text engine adapter (`hcs_engine_llama`): GGUF inference, tokenizer, chat templates, CPU and Vulkan GGML backends |
| stable-diffusion.cpp (Copyright (c) 2023 leejet; `external/stable-diffusion.cpp/LICENSE`) | commit `532d2c15c430a785a712b2d94281f04e733008a4` (submodule `external/stable-diffusion.cpp`), built with `SD_USE_UPSTREAM_GGML=ON` against llama.cpp's GGML | MIT | image engine adapter (`hcs_engine_diffusion`): text-to-image and img2img |
| stb_image / stb_image_write | vendored in `external/stable-diffusion.cpp/thirdparty` | MIT or public domain (dual) | PNG encode, PNG/JPEG decode in `src/engines/diffusion` only |
| libmtmd (`external/llama.cpp/tools/mtmd`, part of llama.cpp, MIT) | same llama.cpp commit; built via `LLAMA_BUILD_MTMD=ON`, `MTMD_VIDEO=OFF` | MIT | image understanding (`LlamaTextEngine::analyze_image`): image decode, vision encoder + mmproj projector |
| stb_image (llama.cpp `vendor/stb`, second copy, symbols renamed `hcs_mtmd_*`) | same llama.cpp commit | MIT or public domain (dual) | PNG/JPEG/BMP/GIF decode inside libmtmd |
| miniaudio, sheredom/subprocess.h, hash (xxhash BSD-2-Clause, SHA-1/SHA-256 public domain, rotate-bits MIT) (llama.cpp `vendor/`) | same llama.cpp commit | public domain / MIT-0 / BSD-2-Clause / MIT | compiled into libmtmd (audio decode, subprocess helper, hashing); audio/video features are not exposed by HCSInfer V1 |
| libwebp (Copyright (c) 2010 Google Inc.; `thirdparty/libwebp/COPYING`) | vendored in `external/stable-diffusion.cpp/thirdparty/libwebp` | BSD-3-Clause | transitive (image encode in stable-diffusion.cpp) |
| oniguruma (Copyright (c) 2002-2021 K.Kosako; `thirdparty/oniguruma/COPYING`) | vendored in `external/stable-diffusion.cpp/thirdparty/oniguruma` | BSD-2-Clause | transitive (tokenizer regex in stable-diffusion.cpp) |
| utf8proc (`thirdparty/utf8proc/LICENSE.md`) | vendored in `external/stable-diffusion.cpp/thirdparty/utf8proc` | MIT | transitive (Unicode handling in stable-diffusion.cpp) |
| GoogleTest | v1.17.0 (commit `52eb8108c5bdec04579160ae17225d66034bd723`), FetchContent | BSD-3-Clause | tests only, not shipped |
| nlohmann/json | v3.12.0 (commit `55f93686c01528224f448c19128836e7df245f72`), FetchContent | MIT | settings/registry/API JSON, linked into HCSInfer |
| cpp-httplib | v0.44.0 (commit `811dd0b6f2382f50f801f3cfdb5ba349d08bb575`), FetchContent (header only, `src/api/http_server.cpp`) | MIT | local HTTP API transport, linked into HCSInfer; no TLS/zlib/brotli features enabled |
| LZ4 | v1.10.0 (commit `ebb370ca83af193212df4dcbadcc5d87bc0de2f0`), FetchContent, static | BSD-2-Clause for the library (`lib/`); the CLI programs under `programs/` are GPL-2.0+ and are NOT built or linked (`LZ4_BUILD_CLI=OFF`) | HCSMODEL chunk codec (`src/storage`) |
| Zstandard | v1.5.7 (commit `f8745da6ff1ad1e7bab384bd1f9d742439278e99`), FetchContent, static | BSD-3-Clause (offered under BSD-3-Clause or GPL-2.0; HCSInfer uses the BSD-3-Clause option) | HCSMODEL chunk codec (`src/storage`) |
| Vulkan-Headers | tag v1.4.357 (commit `e3b1eec08173d6b825cd3ac88c885a63b621504a`), FetchContent in `src/hardware/CMakeLists.txt`, header-only | Apache-2.0 OR MIT (`LICENSE.md`: Copyright 2015-2026 The Khronos Group Inc.) | Vulkan declarations for `hcs_hardware`; `vulkan-1.dll` from the system driver is loaded dynamically. Not yet listed in `external/dependency-lock.json` (carried as a supplemental component by `scripts/sbom.ps1`) |
| prime-agent (Copyright (c) 2025-2026 Prime Intellect Ltd.; `external/prime-agent/LICENSE`) | v0.10.4, commit `8980e66f2f07762d62a6c1d73b347a4cac5275cc` (submodule `external/prime-agent`) | MIT | coding-agent candidate (ADR-011), built separately with cargo as a sidecar (`prime-agent.exe`, `pa-daemon.exe`); NOT linked into `HCSInfer.exe`. Wave G (2026-11-20): all 345 transitive Rust crates and the vendored `crossterm` carry licences within the project's own `deny.toml` allowlist and `cargo audit` was run (findings and reachability in `docs/licence-audit.md`); the literal `cargo deny` run and a Rust SBOM are NOT RUN. |
| @huggingface/transformers (transformers.js; Copyright Hugging Face; Apache-2.0) | **4.3.2**, loaded at run time by `web/` from the pinned jsDelivr URL in `web/js/config.js` | Apache-2.0 | HCS Studio Web browser-inference runtime (ADR-013). Fetched by the visitor's browser; NOT linked into or shipped with `HCSInfer.exe`/the portable package. |
| onnxruntime-web (Copyright (c) Microsoft Corporation) | **1.31.0-dev** (transitive dependency of transformers.js 4.3.2, loaded from the same CDN) | MIT | WebGPU/WASM operator runtime used by the web demo only (transitive). Not present in the native build. |

Not linked: stable-diffusion.cpp's nested patched GGML (`eeda9ec48fc84a3df0bb9dcc19ae9053075a2539`). HCSInfer
builds one GGML in the process (llama.cpp's) and asks stable-diffusion.cpp to use it, so upstream's INT8/FP8
patched paths are unavailable in this configuration (documented limitation, `docs/test-evidence/M1/sd`).

LZ4 and Zstandard licence texts and copyright notices must be reproduced in release packages (`lz4: LICENSE` in
`lib/`, `zstd: LICENSE`). The SHA-256 implementation in `src/storage/sha256.cpp` is original HCSInfer code written
against FIPS 180-4 (the SHA-NI round structure follows the publicly documented Intel instruction sequence).

## Model licences (weights are never distributed or bundled)

Model weights are not distributed with HCSInfer; each model has its own licence, and you must accept it yourself.
This list only records what the repository evidence says; it is not legal advice and a model licence is not a code
licence.

| Model | Licence as recorded in the repository | Source of the statement |
|---|---|---|
| LiquidAI LFM3-Omni-2B Q8_0 + mmproj (default local LLM) | LFM Open License v1.0 (`license: other`, `license_name: lfm1.0`): Apache-style grant; section 5 limits commercial use to legal entities below US$ 10,000,000 annual revenue. Read the model's `LICENSE` before any redistribution. | `docs/test-evidence/lfm3/README.md`, `docs/research/v1.1-model-selection.md` |
| FLUX.3 Image Klein (black-forest-labs, gated; unsloth/leejet GGUF conversions) | FLUX Kommunity License: non-commercial plus a qualifying-output exception; the repository is gated. Qwen3-VL-4B-Instruct (text encoder) and the FLUX.2 VAE are separate files with their own licences that have not been verified here. | `docs/research/v1.1-model-selection.md`, `docs/supported-models.md` |
| SmolVLM2-500M-Video-Instruct Q8_0 + mmproj | Recorded as Apache-2.0 by Hugging Face, **not re-verified**; the download source was not recorded | `docs/test-evidence/M2-vision/README.md` |
| Qwen3-0.6B Q4_K_M (test and benchmark model) | **No licence verification is recorded in the repository** (NOT RUN); check the model card before use | n/a |
| Stable Diffusion 1.5 Q8_0 GGUF (test model) | Unknown provenance and licence (never verified) | `docs/test-evidence/M1/sd/README.md` |


The separate browser deliverable `web/` (HCS Studio Web, ADR-013) loads `@huggingface/transformers` and its
transitive `onnxruntime-web` at run time from a version-pinned CDN URL, inside the visitor's browser. Those bytes
are not part of `HCSInfer.exe`, the portable package or any release artefact, and no model weights they load are
distributed either. Their licences (Apache-2.0 / MIT) are recorded above; the `web/` package itself has no npm
dependencies.

## Licence texts and packaging

`scripts/package.ps1` copies the licence files named above into `licenses/` of the portable package: the
`licence_file` entries of `external/dependency-lock.json` (llama.cpp, stable-diffusion.cpp, libwebp, oniguruma,
utf8proc, stb headers) and the FetchContent licences from the build tree (nlohmann/json `LICENSE.MIT`, cpp-httplib
`LICENSE`, LZ4 `LICENSE`, Zstandard `LICENSE`, Vulkan-Headers `LICENSE.md`). Zstandard's `COPYING` (GPL-2.0) and LZ4's
`programs/` (GPL-2.0+) are not copied or built; HCSInfer uses the BSD options only.

Components present in the submodule trees but NOT compiled into HCSInfer (llama.cpp `vendor/` except the pieces listed above that libmtmd links, `tools/` except `tools/mtmd`, `common`;
stable-diffusion.cpp `examples/` and the bundled patched `ggml`, per the build options in
`external/dependency-lock.json`; other vendored directories were not individually verified as unlinked) are not covered by the notices above. If a build option changes, review this file.
This file is a notice inventory, not legal advice. The formal licence review (AGENTS.md 19.6) is recorded in
`docs/licence-audit.md`: every code dependency linked into or shipped with HCSInfer was verified against its licence
file. Still open there: the literal `cargo deny` run and a Rust SBOM for prime-agent (NOT RUN) and the per-model licence
cards (weights are never distributed).
