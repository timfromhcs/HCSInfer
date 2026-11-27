# w2-linuxci: hosted Linux / macOS / Linux-Vulkan CI bring-up

Branch pushed: `ci/linux-bringup` (3 push iterations of the 12 allowed). Repository: `timfromhcs/HCSInfer`.
Workflow: `.github/workflows/pr.yml` (jobs `linux` matrix = linux-gcc / linux-clang / linux-arm64, `macos-arm64`,
`linux-vulkan-smoke`). Added preset `linux-arm64-release` (the x86 presets set AVX2 flags and cannot be used on aarch64).

## Run table

| Push | Run id | Commit | Result |
|---|---|---|---|
| 1 | 46001487599 | c6363cd | linux-gcc, linux-clang, linux-arm64, macos-arm64 green (963/963 portable tests each; 9 skipped = NOT RUN). linux-vulkan-smoke FAILED at configure: `SPIRV-Headers` package missing. actionlint FAILED: shellcheck SC2012 in new `ls` use and in the pre-existing `pypi.yml` line 64 (the hosted actionlint runs shellcheck; the local Windows binary does not). |
| 2 | 46003458128 | f921077 | Added `spirv-headers`, replaced the `ls` uses. Configure passed; build FAILED: Ubuntu 24.04 apt `glslc` rejects `GL_EXT_integer_dot_product` (`shaders/llm/gemm_i8dot_tiled.comp`). actionlint green. (Run completed only partly relevant; superseded.) |
| 3 | 46005491207 | 47279dc | Pinned LunarG SDK 1.4.363.1 `glslc` + `spirv-val` (SHA-256 verified, cached). **All jobs green**, including linux-vulkan-smoke and the three Windows build+ctest legs. |

Final run 46005491207: every job `success`: windows-msvc-release / -debug / -release-static, clang-format, clang-tidy, web,
python, SBOM, version-consistency, actionlint, linux-gcc, linux-clang, linux-arm64, macos-arm64, linux-vulkan-smoke.

## What the green jobs prove

* gcc 13.3 and clang (ubuntu-24.04), gcc on ubuntu-24.04-arm (aarch64), Apple clang on macos-15 arm64: configure with
  `-Werror` on HCS targets, build, and 963/963 `portable` tests (labels unit|contract|parser|kernel-cpu).
* The two WSL-only flakes (`FsUtil.ConcurrentWritersNeverProduceMixedContent`, `CliRemote.NoServerRunningIsAClearIoErrorNotAStub`)
  did NOT fail on native hosted Linux in any run: they were WSL/drvfs artefacts, no source change was needed.
* No test needed a `WINDOWS_ONLY` exclusion and no test, assertion or backend was changed or weakened.
* linux-vulkan-smoke: `linux-release-vulkan` (GGML Vulkan + HCS Vulkan kernels, shaders compiled and validated by
  glslc/spirv-val) builds, and `HCSInfer doctor --json` reports `vulkan.status = ok` with 1 device:
  `llvmpipe (LLVM 20.1.2, 256 bits)`, Vulkan 1.4.318, type `cpu`.

## What they do NOT prove (NOT RUN)

* Real GPU execution (Radeon 680M, RADV): lavapipe is a CPU Vulkan implementation.
* Real-model inference on Linux/macOS (no model files on hosted runners; tests skip).
* The 9 skipped tests (DoctorLive Vulkan, MemoryProbe GPU heap, OrtRealRuntime, ApiSocketAudit) are counted in each job summary.
* macOS: green once, so the job is still `continue-on-error: true` (one run is not proof of stability). Promote after a few more runs.
* arm64 and macOS binaries were built but not run against real models.

## Changes outside pr.yml / docs/ci.md (flagged for the coordinator)

* `CMakePresets.json`: new `linux-arm64-release` configure/build/test presets.
* `.github/workflows/pypi.yml`: `ls dist | wc -l` -> `find dist -maxdepth 1 -type f | wc -l` (hosted actionlint SC2012; same behaviour).
