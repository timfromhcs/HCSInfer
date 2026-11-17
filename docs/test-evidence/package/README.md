# Portable package (scripts/package.ps1) - first real run

Date: 2026-11-17. Machine: Windows 11 26200, Ryzen 7 7735HS + Radeon 680M, MSVC 19.44.35228, CMake 4.4.0.
Source build: `build/img-api` (Release, CPU-only, `HCS_ENABLE_VULKAN=OFF`), i.e. the build the 400-test suite and the
image/GUI evidence were produced with. Commit at packaging time: `9f03d74134728caa20abcde1771124f37fd6218d`
(recorded inside the generated SBOM).

```
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1 -Preset img-api -Zip
```

Result (exact tool output):

```
SBOM written: ...\HCSInfer-0.1.0-img-api-portable\sbom.cdx.json (13 components, HCSInfer 0.1.0, commit 9f03d74...)
Package: ...\HCSInfer-0.1.0-img-api-portable (20 files hashed)
Zip: ...\HCSInfer-0.1.0-img-api-portable.zip  sha256 527df69376a596a1d4cec77acab192369a0a4fd3ec4be8e8b957a58905b9b1ad
```

The ZIP is 17,992,085 bytes (17.2 MiB) and its SHA-256 was re-computed independently with `Get-FileHash` after the
run: `527df69376a596a1d4cec77acab192369a0a4fd3ec4be8e8b957a58905b9b1ad` (matches).

## Contents (21 files: 9 at the top level, 12 licence texts, plus `licenses/`)

`HCSInfer.exe`, `README.md`, `SECURITY.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `manifest.json`,
`PACKAGE-README.txt`, `SHA256SUMS.txt`, `sbom.cdx.json` and `licenses/` with 12 licence texts:
`cpp_httplib-LICENSE`, `libwebp-COPYING`, `llama.cpp-LICENSE`, `lz4-LICENSE`, `nlohmann_json-LICENSE.MIT`,
`oniguruma-COPYING`, `stable-diffusion.cpp-LICENSE`, `stable-diffusion.cpp_bundled_patched_GGML-LICENSE`,
`stb_image___stb_image_write-stb_image.h`, `utf8proc-LICENSE.md`, `vulkan_headers-LICENSE.md`, `zstd-LICENSE`.
`SHA256SUMS.txt` hashes 20 files (the manifest itself is excluded); copies of `SHA256SUMS.txt`, `manifest.json` and
`PACKAGE-README.txt` are kept next to this file. The manifest states `single_exe: false`, `crt: dynamic (/MD, default)`,
`signed: false` and the hash of the source executable.

No test executable, no storage benchmark and no build tree: the package copies only the runtime binary (and any DLL
next to it - there were none).

## The packaged binary runs standalone

```
cd ...\HCSInfer-0.1.0-img-api-portable
.\HCSInfer.exe doctor --json      -> exit 0, full JSON report
```

The complete JSON is next to this file (`doctor-from-package.json`); it reports the same CPU features (AVX2/FMA3/
BMI1/BMI2/AES/F16C), OS build and RAM as the in-tree binary.

## Honest limitations

* **The clean-machine test has NOT been run.** The binary was executed from the package directory on this machine,
  which has Visual Studio 2022 and therefore the MSVC runtime installed. The package uses the **dynamic MSVC runtime**
  (`/MD`, ADR-008), so a machine without the VC++ redistributable would need it installed; that claim is unverified
  here either way and must be tested before any release statement.
* This is a **portable folder / ZIP**, not a single-file EXE. The one-EXE gate (M7) remains NOT RUN.
* No signing: the ZIP and the EXE are unsigned; `SHA256SUMS.txt` covers integrity, not authenticity.
* `-Preset img-api` was used because it names the verified build directory; the script only needs a directory under
  `build/` with `bin/HCSInfer.exe`, and it refuses any path outside `<repo>/artifacts/package`.
* The SBOM is generated from `external/dependency-lock.json` plus the build tree's FetchContent licences; the
  transitive model-weights story is out of scope (models are never bundled).
