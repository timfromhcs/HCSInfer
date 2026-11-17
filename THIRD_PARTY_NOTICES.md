# Third-party notices (updated 2026-11-17 from the submodule and fetched LICENSE files; SBOM: `scripts/sbom.ps1`)

Dependencies linked into or fetched by the current build. Exact pins and CMake options:
`external/dependency-lock.json`. Licence texts must be reproduced in release packages.

| Component | Version / pin | Licence | Use |
|---|---|---|---|
| llama.cpp / GGML (Copyright (c) 2023-2026 The ggml authors; `external/llama.cpp/LICENSE`) | commit `d3c3e242c16b8cbf51400fad410cd69b22d64f35` (submodule `external/llama.cpp`) | MIT | text engine adapter (`hcs_engine_llama`): GGUF inference, tokenizer, chat templates, CPU and Vulkan GGML backends |
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
| cpp-httplib | v0.37.2 (commit `4d7c9a788de136071ccf0dd4e96239151e2adadb`), FetchContent (header only, `src/api/http_server.cpp`) | MIT | local HTTP API transport, linked into HCSInfer; no TLS/zlib/brotli features enabled |
| LZ4 | v1.10.0 (commit `ebb370ca83af193212df4dcbadcc5d87bc0de2f0`), FetchContent, static | BSD-2-Clause for the library (`lib/`); the CLI programs under `programs/` are GPL-2.0+ and are NOT built or linked (`LZ4_BUILD_CLI=OFF`) | HCSMODEL chunk codec (`src/storage`) |
| Zstandard | v1.5.7 (commit `f8745da6ff1ad1e7bab384bd1f9d742439278e99`), FetchContent, static | BSD-3-Clause (offered under BSD-3-Clause or GPL-2.0; HCSInfer uses the BSD-3-Clause option) | HCSMODEL chunk codec (`src/storage`) |
| Vulkan-Headers | tag v1.4.357 (commit `e3b1eec08173d6b825cd3ac88c885a63b621504a`), FetchContent in `src/hardware/CMakeLists.txt`, header-only | Apache-2.0 OR MIT (`LICENSE.md`: Copyright 2015-2026 The Khronos Group Inc.) | Vulkan declarations for `hcs_hardware`; `vulkan-1.dll` from the system driver is loaded dynamically. Not yet listed in `external/dependency-lock.json` (carried as a supplemental component by `scripts/sbom.ps1`) |
| prime-agent (Copyright (c) 2025-2026 Prime Intellect Ltd.; `external/prime-agent/LICENSE`) | v0.10.4, commit `8980e66f2f07762d62a6c1d73b347a4cac5275cc` (submodule `external/prime-agent`) | MIT | coding-agent candidate (ADR-011), built separately with cargo as a sidecar (`prime-agent.exe`, `pa-daemon.exe`); NOT linked into `HCSInfer.exe`. Its transitive Rust crates and the vendored `crossterm` are not audited yet; `cargo deny` / SBOM for them is NOT RUN. |

Not linked: stable-diffusion.cpp's nested patched GGML (`eeda9ec48fc84a3df0bb9dcc19ae9053075a2539`). HCSInfer
builds one GGML in the process (llama.cpp's) and asks stable-diffusion.cpp to use it, so upstream's INT8/FP8
patched paths are unavailable in this configuration (documented limitation, `docs/test-evidence/M1/sd`).

LZ4 and Zstandard licence texts and copyright notices must be reproduced in release packages (`lz4: LICENSE` in
`lib/`, `zstd: LICENSE`). The SHA-256 implementation in `src/storage/sha256.cpp` is original HCSInfer code written
against FIPS 180-4 (the SHA-NI round structure follows the publicly documented Intel instruction sequence).

Model weights are not distributed with HCSInfer; each model has its own licence. The SD1.5 Q8_0 GGUF used in
local tests has no verified provenance and is never redistributed.

## Licence texts and packaging

`scripts/package.ps1` copies the licence files named above into `licenses/` of the portable package: the
`licence_file` entries of `external/dependency-lock.json` (llama.cpp, stable-diffusion.cpp, libwebp, oniguruma,
utf8proc, stb headers) and the FetchContent licences from the build tree (nlohmann/json `LICENSE.MIT`, cpp-httplib
`LICENSE`, LZ4 `LICENSE`, Zstandard `LICENSE`, Vulkan-Headers `LICENSE.md`). Zstandard's `COPYING` (GPL-2.0) and LZ4's
`programs/` (GPL-2.0+) are not copied or built; HCSInfer uses the BSD options only.

Components present in the submodule trees but NOT compiled into HCSInfer (llama.cpp `vendor/` except the pieces listed above that libmtmd links, `tools/` except `tools/mtmd`, `common`;
stable-diffusion.cpp `examples/` and the bundled patched `ggml`, per the build options in
`external/dependency-lock.json`; other vendored directories were not individually verified as unlinked) are not covered by the notices above. If a build option changes, review this file.
This file is a notice inventory, not legal advice; a formal licence review (AGENTS.md 19.6) is still open (NOT RUN).
