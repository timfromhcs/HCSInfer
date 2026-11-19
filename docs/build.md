# Build, test and module conventions

Known-good environment (verified 2026-11-17 on the target machine): Windows 11 10.0.26200, MSVC 19.44.35228
(Visual Studio 2022 Community), CMake 4.4.0, Ninja 1.13.2, GoogleTest v1.17.0, nlohmann/json v3.12.0.

## Presets

`CMakePresets.json` defines the configure/build/test presets `windows-msvc-debug`, `windows-msvc-release` (both CPU,
`HCS_ENABLE_VULKAN=OFF`), `windows-msvc-release-vulkan` (needs the LunarG Vulkan SDK at build time) and
`windows-msvc-release-static` (static CRT `/MT` plus `GGML_OPENMP=OFF` for the single-EXE experiment, ADR-008) and
`windows-msvc-release-kernels` (the experimental HCS Vulkan kernels + CPU reference, M8; needs the Vulkan SDK's
`glslc`/`spirv-val` at build time). All use Ninja and `cl`. There are no Linux or ASan presets yet. Local test builds of
this project also used ad-hoc build directories (for example `build/img-api`); those are not presets.

## Commands

All commands from the repository root. The presets use the Ninja generator and `cl`, so they need the MSVC
x64 environment. `scripts/build.ps1` loads it automatically; for manual use run from a "x64 Native Tools"
prompt or `call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"`.

```powershell
# scripted (recommended)
.\scripts\build.ps1 -Preset windows-msvc-debug -RunTests
.\scripts\build.ps1 -Preset windows-msvc-release -RunTests
.\scripts\build.ps1 -Preset windows-msvc-release-vulkan      # same as -Vulkan; only sets HCS_ENABLE_VULKAN=ON for now
.\scripts\build.ps1 -Preset windows-msvc-debug -CleanBuildDir # removes only <repo>/build/<preset> (path-guarded)
.\scripts\test.ps1  -Preset windows-msvc-debug -Label unit    # labels: unit, contract, ...
.\scripts\doctor.ps1 -Preset windows-msvc-release -Json

# web/ (HCS Studio Web) is separate and needs only Node: cd web; npm test; node tests/e2e/boot.mjs

# manual equivalent (inside an MSVC x64 environment)
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug --output-on-failure
ctest --preset windows-msvc-debug -N                 # list discovered tests
ctest --preset windows-msvc-debug -L contract        # by label
```

Binary: `build/<preset>/bin/HCSInfer.exe`. Test executables: `build/<preset>/bin/hcs_test_<name>.exe`.
Build directories live under `build/` (git-ignored). The first configure needs network access to fetch the
pinned GoogleTest and nlohmann/json tags.

CMake options: `HCS_BUILD_TESTS` (ON), `HCS_WARNINGS_AS_ERRORS` (ON, HCS targets only), `HCS_ENABLE_VULKAN` (OFF).

## Module conventions (for other engineers)

The top-level `CMakeLists.txt` discovers modules; you normally do not edit it.

1. **Location:** `src/<module>/CMakeLists.txt` for one of the pre-registered names
   `core hardware storage memory scheduler telemetry process agent train engines/llama engines/diffusion gui api mcp cli app`
   (`memory`, `scheduler` and `telemetry` have no sources yet).
   Discovery order is exactly that list (cli and app last). A brand-new module name needs ONE added word in
   `HCS_MODULE_DIRS` in the top-level file.
2. **Target name:** `hcs_<name>` (static library) with alias `hcs::<name>`; for `engines/llama` use
   `hcs_engine_llama`. Create it with the helper:
   ```cmake
   hcs_add_library(hardware
     SOURCES cpu_info.cpp vulkan_info.cpp doctor.cpp
     PUBLIC_LINK hcs_core
     PRIVATE_LINK nlohmann_json::nlohmann_json)
   ```
   This applies C++20, the `hcs_warnings` flags (/W4 /WX) and the include roots `include/` and
   `${CMAKE_BINARY_DIR}/generated`. Third-party code (upstream engines) must NOT link `hcs_warnings`.
3. **Public headers:** `include/hcs/<module>/<header>.hpp` (e.g. `include/hcs/hardware/doctor.hpp`); private
   headers stay next to the sources. Public headers must not expose upstream types (`llama_*`, `ggml_*`, `sd_*`, Vulkan handles).
4. **Optional-module delegation:** `src/cli/CMakeLists.txt` links `hcs_hardware` and defines
   `HCS_HAVE_HARDWARE=1` when `TARGET hcs_hardware` exists. The hardware module must therefore provide
   `include/hcs/hardware/doctor.hpp` declaring
   `namespace hcs::hardware { int run_doctor(bool json, std::ostream& out); }` (returns an hcs exit code;
   JSON schema_version 1; unknown values are the string `"unknown"`). The same pattern (`if(TARGET hcs_x)`
   in `src/cli`, `src/api`, `src/mcp`) is used for later modules; engine adapters must implement
   `hcs::ITextEngine` / `hcs::IImageEngine` from `include/hcs/engine.hpp`.
5. **Vulkan/engine modules:** honour `HCS_ENABLE_VULKAN`; upstream submodules are added inside the module's own
   CMakeLists (`add_subdirectory(${CMAKE_SOURCE_DIR}/external/llama.cpp ...)`) with their own option scoping, not at the top level.
6. **Tests:** create `tests/<layer>/<name>/CMakeLists.txt` (layers: `unit contract parser-fuzz kernels integration e2e performance soak`);
   it is discovered automatically (`file(GLOB ... CONFIGURE_DEPENDS)`). Use:
   ```cmake
   hcs_add_gtest(hardware
     SOURCES test_cpu_info.cpp
     LINK hcs_hardware hcs_core
     LABELS unit)            # becomes executable hcs_test_hardware, tests carry CTest labels
   ```
   `tests/support/temp_dir.hpp` (TempDir, write_file, fake_gguf, make_dir_link) is on the include path.
   Tests needing real hardware/models use labels `integration`/`e2e` and must report BLOCKED/skip, never fake a pass.
7. **Exit codes / errors:** return `hcs::Result<T>` with an `hcs::ErrorCode`; the CLI maps it to the stable exit code.
8. **Third-party libs:** pin by exact tag in `cmake/*.cmake` (FetchContent) and record the commit and licence in `THIRD_PARTY_NOTICES.md`.

## Troubleshooting

- `cl` not found when running `cmake --preset ...` manually: initialise the MSVC environment (see above).
- Configure fails fetching dependencies: check network/proxy; nothing is fetched at runtime.
- A stale cache after changing generator/compiler: delete only `build/<preset>` (`-CleanBuildDir`).
- Symlink tests are skipped (not failed) if the OS refuses both symlink and junction creation.

## Format, SBOM, package, CI

```powershell
.\scripts\format.ps1            # check only (clang-format --dry-run --Werror); -Fix rewrites src/include/tests/tools
.\scripts\static-analysis.ps1   # clang-tidy over src (clang-analyzer-*, bugprone-*); -Scope all, -FailOnFindings
.\scripts\sbom.ps1              # CycloneDX JSON from external/dependency-lock.json -> artifacts\sbom\hcsinfer.cdx.json
.\scripts\package.ps1 -Preset windows-msvc-release -Zip   # PORTABLE FOLDER (not single-EXE) + licenses + SBOM + SHA256SUMS.txt
```

`package.ps1` packages an existing build; it does not build, sign or test. Output goes to `artifacts\package\`
(git-ignored). The package uses the dynamic MSVC runtime (ADR-008).

**Tool versions matter for both analysers.** `clang-format` and `clang-tidy` are version-dependent, so
`Find-VsLlvmTool` (in `scripts/common.ps1`) prefers the tool bundled with **Visual Studio 2022** - the same source the
`windows-latest` CI runner has - and only then falls back to PATH or `C:\Program Files\LLVM`. On this development
machine PATH additionally has a ROCm 21.0.0git clang-format and an LLVM 22.1.8 install, either of which formats and
reports differently.

Formatting status: the whole tree is clang-formatted as of 2026-11-17 (139 files, clang-format **19.1.5**, the VS
2022-bundled build; `format.ps1` reports OK). `.clang-format` sets `SortIncludes: Never` and `IncludeBlocks: Preserve`
because several translation units depend on include order (`windows.h` before `psapi.h`; `NOMINMAX` before
`windows.h`). The CI format job is blocking (confirmed green on the runner); if a future runner image changes its bundled LLVM
the job can fail on pure re-wrapping, which is then handled deliberately.

clang-tidy: `static-analysis.ps1` loads the Visual Studio 2022 environment before analysing, because clang resolves the
MSVC STL through the registry and would otherwise pick the newest Visual Studio on the machine - a VS 18 BuildTools STL
requires Clang 20+ and fails with `STL1000` under the VS 2022-bundled clang-tidy. `/WX` from the HCS targets is
neutralised with `-Wno-error` so the analysis always completes; only the checks named in `-Checks` are reported.

CI (`.github/workflows/`): `pr.yml` (`pr-quick-gate`) = hosted Windows quick gate on pull requests and pushes to
`main`. Six jobs, all blocking: `build + ctest` for the Release and Debug CPU presets (no models: model tests are
reported as skipped = NOT RUN), `clang-format check`, `clang-tidy (static analysis)` (`static-analysis.ps1 -Scope src
-FailOnFindings`), `SBOM generation + schema sanity`, and `web` (`npm test` plus the headless Chrome boot check).
Green on `main`: run 44167798984 (2026-11-17, commit `d8a5e37`). `hardware.yml` = self-hosted Radeon 680M runner for
nightly / manual / `v*` tag runs only, never for pull requests; it has never been executed (NOT RUN).
