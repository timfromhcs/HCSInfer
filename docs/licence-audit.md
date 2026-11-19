# Licence / dependency audit (AGENTS.md 7.3 and 19.6)

Status: **code dependencies verified for the shipped artefacts; two gaps remain** (the prime-agent transitive Rust
crate tree, and the model cards — see below). This closes the "formal licence review (AGENTS.md 19.6) is still open"
item in `THIRD_PARTY_NOTICES.md` down to those two named gaps. It is a record of what was checked, not legal advice.

Date: 2026-11-19. Pins: `external/dependency-lock.json`. The notice inventory: `THIRD_PARTY_NOTICES.md`.

## Method

For every component that is **linked into `HCSInfer.exe`**, **shipped in the portable package**, or **fetched into
the build tree**, the licence file was read in place (submodule or `build/<preset>/_deps/`), the SPDX identifier was
recorded, and the obligation was checked:

* MIT / BSD-2 / BSD-3 / public domain: a redistribution needs the copyright + licence text. `scripts/package.ps1`
  copies each named `licence_file` and each FetchContent licence into `licenses/` of the package, alongside
  `THIRD_PARTY_NOTICES.md`.
* Apache-2.0: needs the licence text and, if the component ships one, its `NOTICE` file.
* GPL / copyleft: must not be built or shipped. Only LZ4's `programs/` and Zstandard's `COPYING` are GPL; both are
  excluded from the build (`LZ4_BUILD_CLI=OFF`, `LZ4_BUILD_LEGACY_LZ4C=OFF`) and from the package.
* Header-only components (cpp-httplib, Vulkan-Headers): no compiled bytes are redistributed; the licence text is
  still copied for completeness.

## Verified components (linked or shipped in the native build)

| Component | Pin | Licence (verified from the file) | Obligation | State |
|---|---|---|---|---|
| llama.cpp / GGML | `d3c3e242…` | MIT (`external/llama.cpp/LICENSE`) | keep copyright + licence | PASS (copied) |
| stable-diffusion.cpp (+ its nested GGML, not linked) | `532d2c15…` / `eeda9ec4…` | MIT | keep copyright + licence | PASS (copied) |
| stb_image / stb_image_write | vendored in sd.cpp `thirdparty/` | MIT or public domain (dual; `stb_image.h` says "public domain") | none if public domain elected | PASS |
| libwebp | vendored | BSD-3-Clause (`COPYING`) | keep copyright + licence | PASS (copied) |
| oniguruma | vendored | BSD-2-Clause (`COPYING`) | keep copyright + licence | PASS (copied) |
| utf8proc | vendored | MIT (`LICENSE.md`) | keep copyright + licence | PASS (copied) |
| nlohmann/json | `55f93686…` | MIT (`LICENSE.MIT`) | keep copyright + licence | PASS (copied) |
| cpp-httplib | `4d7c9a78…` | MIT (`LICENSE`) | keep copyright + licence | PASS (copied) |
| LZ4 (library) | `ebb370ca…` | BSD-2-Clause (`LICENSE`: "all files in the `lib` directory use a BSD 2-Clause license") | keep copyright + licence; GPL `programs/` excluded | PASS (copied; CLI off) |
| Zstandard | `f8745da6…` | BSD-3-Clause (`LICENSE` "BSD License"); `COPYING` is GPL-2.0 and is not copied | keep copyright + licence | PASS (copied; BSD option) |
| Vulkan-Headers | `e3b1eec0…` | **Apache-2.0 OR MIT** (`LICENSE.md`; per-file SPDX). No `NOTICE` file exists in the tree. | keep licence; with no NOTICE, Apache-2.0 §4(d) has nothing to propagate | PASS (copied) |
| GoogleTest | `52eb8108…` | BSD-3-Clause (`LICENSE`) | tests only, not shipped | PASS (not shipped) |

## Verified components (not linked, but in the repository)

| Component | Pin | Licence (verified) | State |
|---|---|---|---|
| prime-agent | `8980e66f…` (v0.10.4) | MIT, "Copyright (c) 2025-2026 Prime Intellect Ltd." — **verified at the pinned commit** via the upstream raw URL, because the `external/prime-agent` submodule is **not initialised in this working tree** (`git submodule status` shows a leading `-`) | PASS for the licence; its **transitive Rust crates and the vendored `crossterm` are NOT audited** (`cargo deny` / SBOM for them NOT RUN) |
| @huggingface/transformers (transformers.js) | 4.3.2, loaded at run time by `web/` | Apache-2.0 | PASS (browser only; not shipped) |
| onnxruntime-web | 1.31.0-dev (transitive of the above) | MIT | PASS (browser only; not shipped) |

## Findings

1. **`Vulkan-Headers` is `Apache-2.0 OR MIT`.** HCSInfer effectively elects **MIT** (simplest obligation) and there
   is no `NOTICE` file in the component, so nothing extra must be propagated. `scripts/package.ps1` copies
   `LICENSE.md`. The HCS M8 kernels use it at build time only; `vulkan-1.dll` is loaded from the system driver.
2. **`prime-agent`'s licence file is not present in the working tree** (submodule not initialised). The licence was
   verified against the pinned commit instead. This is recorded so a release checklist does not mistake "file absent
   locally" for "no licence".
3. **GPL components are present in the build tree but not built or shipped**: Zstandard's `COPYING` (GPL-2.0) and
   LZ4's `programs/` (GPL-2.0+). Verified: `LZ4_BUILD_CLI=OFF`, `ZSTD_BUILD_PROGRAMS=OFF`, and `package.ps1` copies
   only the BSD `LICENSE` files.
4. **No new dependency was added by the M8 kernel work** (`shaders/llm/*.comp`, `src/kernels/*`): GLSL is compiled by
   the build-only Vulkan SDK `glslc`; no third-party shader code is vendored, and the shaders are original HCSInfer
   code.

## Remaining gaps (NOT RUN — must be closed before a public release)

* **prime-agent transitive Rust crate tree** (ADR-011): `cargo deny`/SBOM not run. Owner-side because it needs the
  initialised submodule and the Rust toolchain.
* **Model weights/cards**: HCSInfer never distributes weights. The per-model licence status (some unverified —
  Qwen3-0.6B, SD1.5) is tracked in `THIRD_PARTY_NOTICES.md` and is the user's responsibility to accept.
* **`web/` runtime CDN bytes**: licences recorded; not shipped. The web deliverable's own dependency graph is empty
  (no npm deps), so no further audit is required for it.
