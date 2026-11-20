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
| cpp-httplib | `811dd0b6…` (v0.44.0; was v0.37.2 `4d7c9a78…`, see Findings 6) | MIT (`LICENSE`) | keep copyright + licence | PASS (copied) |
| LZ4 (library) | `ebb370ca…` | BSD-2-Clause (`LICENSE`: "all files in the `lib` directory use a BSD 2-Clause license") | keep copyright + licence; GPL `programs/` excluded | PASS (copied; CLI off) |
| Zstandard | `f8745da6…` | BSD-3-Clause (`LICENSE` "BSD License"); `COPYING` is GPL-2.0 and is not copied | keep copyright + licence | PASS (copied; BSD option) |
| Vulkan-Headers | `e3b1eec0…` | **Apache-2.0 OR MIT** (`LICENSE.md`; per-file SPDX). No `NOTICE` file exists in the tree. | keep licence; with no NOTICE, Apache-2.0 §4(d) has nothing to propagate | PASS (copied) |
| GoogleTest | `52eb8108…` | BSD-3-Clause (`LICENSE`) | tests only, not shipped | PASS (not shipped) |

## Verified components (not linked, but in the repository)

| Component | Pin | Licence (verified) | State |
|---|---|---|---|
| prime-agent | `8980e66f�` (v0.10.4) | MIT, "Copyright (c) 2025-2026 Prime Intellect Ltd." � verified in the file after the submodule was initialised at the pin (wave G) | PASS. **Transitive Rust crates (wave G):** 345 registry crates + 11 workspace members (incl. the vendored `crossterm`, MIT with its own `LICENSE`) � every licence expression is satisfiable by the allowlist in the project's own `deny.toml` (0 mismatches; `docs/test-evidence/wave-g/cargo-licences-prime-agent.txt`). `cargo audit` findings: see Findings 5 |
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

5. **Supply-chain scan, wave G (2026-11-20).** Tools installed from the official crates.io registry with
   `cargo install cargo-audit --locked` (cargo-audit 0.23.0, RustSec DB of 1550 advisories fetched the same day);
   `npm audit` from npm 11.21.1. Evidence: `docs/test-evidence/wave-g/`.
   * `web/` has no npm dependencies: `npm audit` = 0 vulnerabilities. The only third-party code the web app loads
     (`@huggingface/transformers@4.3.2` and its transitive tree, incl. `onnxruntime-web 1.31.0-dev`) was audited in
     a scratch project resolving that exact pin: **0 vulnerabilities**.
   * `external/prime-agent/Cargo.lock` (356 crates): `cargo audit` reports **4 vulnerabilities and 3 allowed
     warnings**. (a) `RUSTSEC-2026-0523`, `-0524` (quinn-proto 0.11.18) and `-0538` (quinn-udp 0.5.15) are present in
     `Cargo.lock` only: `cargo tree --all-features --target all -e all` shows **no `quinn*` node**, so no QUIC
     code is compiled into any prime-agent binary (reqwest's optional HTTP/3 feature is not enabled). Not
     reachable; to be re-checked when the pin moves. (b) `RUSTSEC-2023-0071` (rsa 0.9.10, Marvin timing side
     channel, no upstream fix) is reachable in the graph (`pa-ai` declares it; `pa-cli` uses it in tests, `pa-core`
     decrypts one local OAEP login result per fresh key). prime-agent triages it in its own `deny.toml`
     (reviewed 2026-11-11); HCSInfer inherits that reasoning because it never runs the Prime browser login (the
     agent talks to the loopback endpoint only, ADR-011). (c) Warnings `RUSTSEC-2024-0436` (paste, unmaintained)
     and `RUSTSEC-2026-0002`/`-0253` (lru 0.12.5 via ratatui, unsound) are the other upstream-triaged entries.
     No HCSInfer code or configuration change was required; the sidecar stays a build-it-yourself candidate and is
     not shipped in the portable package.

6. **Native pins vs OSV.dev (wave G, 2026-11-20): cpp-httplib had 9 advisories at the old pin � fixed by a dedicated
   pin bump.** `POST https://api.osv.dev/v1/query` with each pinned commit SHA (public data only;
   `docs/test-evidence/wave-g/osv-native-deps.txt`): llama.cpp, stable-diffusion.cpp (+ its GGML), GoogleTest,
   nlohmann/json, LZ4, Zstandard, prime-agent: no advisory mapped (this is **not** proof of absence � OSV coverage of
   C/C++ pins is incomplete, and stb/libwebp/oniguruma/utf8proc are vendored without a commit in the lock, so they
   were not queried: NOT RUN). **cpp-httplib v0.37.2 (`4d7c9a78`) was mapped to 9 CVEs.** Four are in the *server*
   request path HCSInfer exposes on loopback: CVE-2026-34441 (request smuggling via an unconsumed GET body),
   CVE-2026-45352 (negative chunk size in chunked Transfer-Encoding, DoS), CVE-2026-45372 (header percent-decoding
   CRLF injection) and CVE-2026-46527 (X-Forwarded-For under a trusted-proxy configuration; HCSInfer configures no
   trusted proxy). The other five are client/TLS/WebSocket-client issues (cross-origin redirect credential leaks,
   CRLF in `open_stream`, TLS verification on Mbed TLS/wolfSSL, WebSocketClient use-after-free) in code HCSInfer
   never compiles into a path it uses (server only, no TLS backend, no client, no WebSocket). Action: pin moved to
   **v0.44.0 (`811dd0b6`)**, the first tag containing the server-side fixes (`git tag --contains` on the three fix
   commits), as its own compatibility change (AGENTS.md 0.2): the HCS access-specifier patch still applies (the
   configure-time guard found the declaration), the full Release suite is unchanged (814 registered, 778 passed, 36
   skipped = NOT RUN, 0 failed; `ctest-release-cpu-httplib-0.44.txt`) including the 30 real-socket tests of `hcs_test_api_http`. OSV after the
   bump still lists CVE-2026-102944, -103930, -54919, -77358 � all in the client/TLS/WebSocket code paths above, not
   reachable here; moving to a tag with those fixes needs a larger jump (v0.4x -> v0.6x) and is deferred to the next
   dependency change. The Debug/Vulkan/static presets and CI fetch the pin from `cmake/HcsHttplib.cmake`; only the
   Release CPU preset was re-run in this wave (the others are NOT RUN against v0.44.0).

## Remaining gaps (NOT RUN — must be closed before a public release)

* **prime-agent transitive Rust crate tree** (ADR-011): `cargo deny`/SBOM not run. Owner-side because it needs the
  initialised submodule and the Rust toolchain.
* **Model weights/cards**: HCSInfer never distributes weights. The per-model licence status (some unverified —
  Qwen3-0.6B, SD1.5) is tracked in `THIRD_PARTY_NOTICES.md` and is the user's responsibility to accept.
* **`web/` runtime CDN bytes**: licences recorded; not shipped. The web deliverable's own dependency graph is empty
  (no npm deps), so no further audit is required for it.
