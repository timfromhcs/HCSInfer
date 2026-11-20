# Wave R (release candidate 0.1.0) - results

Date 2026-11-20. Machine: Ryzen 7 7735HS, Radeon 680M, Windows 11 26200, MSVC 19.44.35228, CMake 4.4.0 (the
worktree is mapped to `Y:` because the Vulkan sub-build exceeds MAX_PATH). Source commit under test: `4129b4d`
(formatting + docs) on top of `6f68ff6`.

| Gate | Result | Evidence |
|---|---|---|
| `scripts/format.ps1` (clang-format 19.1.5) | RED before (4 wave-K kernel files), GREEN after `-Fix`: 221 files | `format-red-before.txt`, `format-check.txt` |
| Release build `windows-msvc-release` | PASS (up to date, `/W4 /WX`) | `build-release.txt` |
| `scripts/static-analysis.ps1 -Scope src -FailOnFindings` | PASS, 0 diagnostics, 69 TUs (run from `Y:` so the compile database paths match; the Vulkan-kernel TUs are not in the Release database) | `clang-tidy.txt` |
| Full CTest `windows-msvc-release` (CPU, `-j4`) | PASS: 814 registered, 778 passed, 36 skipped (NOT RUN: need a model/GPU, env `HCS_TEST_*` unset), 0 failed | `ctest-release-cpu.txt` |
| `windows-msvc-release-kernels`: `kernel-cpu` + `kernel-vulkan` (rebuilt after the reformat) | PASS 12/12 on the real Radeon 680M | `ctest-kernels.txt` |
| `web/` `npm test` | PASS 46/46 | `npm-test.txt` |
| `scripts/package.ps1 -Zip` | PASS: portable folder (19 hashed files + manifest, 11 licence texts), ZIP 18,168,288 bytes, SHA-256 `55850ed9cecc75cb6bf44a608fb643b70d2904db961e8d505e36493c29da2380` | `package-run.txt`, `package-verify.txt`, `package-zip.sha256` |
| `SHA256SUMS.txt` verification | PASS: 19 entries, 0 mismatches; ZIP hash re-computed and equal | `package-verify.txt` |
| SBOM | CycloneDX 1.5, 14 components, commit `4129b4d` | `package-sbom.cdx.json` |
| Packaged `HCSInfer.exe doctor --json` | PASS (exit 0) | `package-verify.txt` |

## Defect found and fixed in wave R

`scripts/package.ps1` looked for the FetchContent licence texts only under `<build>/_deps`. With the offline
`FETCHCONTENT_SOURCE_DIR_*` overrides used on this machine the sources live elsewhere, so the package silently lacked
the cpp-httplib, lz4, zstd and nlohmann_json licence texts (warnings only). The script now falls back to the override
path recorded in `CMakeCache.txt`; all four are now in `licenses/`.

## Caveats (not hidden)

- This worktree's nested submodules of `stable-diffusion.cpp` (`ggml`, `thirdparty/libwebp`, ...) are not initialised
  (the main checkout's copy is shallow, a clone from the network was not attempted). Consequently stable-diffusion.cpp
  defaulted `SD_WEBP=OFF` here, i.e. this local package has no WebP support, and its `licenses/` lacks the libwebp and
  patched-GGML texts (the patched GGML is not linked: `SD_USE_UPSTREAM_GGML=ON`). The SBOM lists libwebp as a linked
  component because it is generated from the lock file, not from the build graph; a CI package (recursive checkout)
  does include libwebp. Treat the SBOM/libwebp line as conservative, not as a statement about this binary.
- Model-dependent tests (36) were NOT RUN in this wave; their last real-model evidence is in earlier waves.
- No clean-machine test, no signing, no tag, no merge, no push.
