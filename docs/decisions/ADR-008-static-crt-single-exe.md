# ADR-008: Static CRT and the single-EXE target

- Status: Accepted as a staged plan (2026-11-17). The single-EXE claim is NOT made.
- Date: 2026-11-17

## Context
AGENTS.md 9.5 and M7 want one `HCSInfer.exe` (static MSVC CRT) if every linked dependency allows it. Models and the
system Vulkan driver stay external. Current state, inspected: the top-level CMake and `cmake/` set no
`CMAKE_MSVC_RUNTIME_LIBRARY` and no `CMP0091` policy, so every target uses CMake's default dynamic CRT (`/MD`).
Linked third-party code: llama.cpp/GGML, stable-diffusion.cpp (with libwebp, oniguruma, utf8proc, stb), nlohmann/json,
cpp-httplib (header only), LZ4, Zstandard. No shader assets are embedded (HCS ships no shaders yet).

## Options
1. Keep `/MD` and ship a portable folder, requiring the Visual C++ redistributable on the target machine.
2. `/MT` (`MultiThreaded`, Debug: `MultiThreadedDebug`) for all targets including upstream, set via
   `CMAKE_MSVC_RUNTIME_LIBRARY` before any target exists, with `cmake_policy(SET CMP0091 NEW)`.
3. `/MT` only for HCS targets (rejected: mixing CRTs across static libraries causes link errors or two heaps).

## Decision
- Ship the portable folder package (option 1) for the current alpha: `scripts/package.ps1` produces it and is explicit
  that it is not a single-file build.
- Pursue option 2 as an experiment on a dedicated preset (`windows-msvc-release-static`, **created 2026-11-18**:
  `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` + `GGML_OPENMP=OFF`) because it is the only way to a one-EXE release.
  Promote it only when all of the following have passed: the full test suite with `/MT`; real llama and SD1.5 runs on
  CPU and Vulkan; the binary starts on a Windows install without the VC++ redistributable and without the Vulkan SDK
  (clean-machine test); `dumpbin /dependents` shows only system DLLs; licence notices complete.
- Do not label any release "single-file" before those results exist.

## Rejected
Setting `/MT` globally by editing upstream CMake files; forcing it in a way that breaks upstream's own options.

## Risks
Upstream may link something that is incompatible with `/MT` (OpenMP, prebuilt third-party libs); `/MT` increases binary
size; the static CRT and thread/locale behaviour must be retested. Vulkan loading (`vulkan-1.dll`) stays dynamic by design.

## Evidence
Static CRT **experiment run on 2026-11-18** (MSVC 19.44.35228, CMake 4.4.0, Ninja 1.13.2), evidence in
`docs/test-evidence/W4-static-exe/`:

* Preset `windows-msvc-release-static` (`/MT` everywhere, `GGML_OPENMP=OFF`) configures and builds a fresh tree
  (448 targets). **Full Release CTest: 783/783, 0 failed** (CPU-only build; model-dependent and Vulkan tests skip =
  NOT RUN). `HCSInfer.exe --version` and `doctor --json` run standalone.
* `dumpbin /dependents` shows **only system DLLs**: `ADVAPI32.dll`, `WS2_32.dll`, `KERNEL32.dll`. The default `/MD`
  build also imports `MSVCP140.dll`, `VCRUNTIME140*.dll`, `VCOMP140.DLL` and the `api-ms-win-crt-*` UCRT stubs, i.e.
  it needs the Visual C++ Redistributable.
* The one finding: with the default `GGML_OPENMP=ON`, ggml links `vcomp140.dll` (MSVC OpenMP, from the redistributable)
  even under `/MT`. Disabling it (`GGML_OPENMP=OFF`, ggml then uses its own thread pool) is what makes the EXE
  system-DLL-only. CPU throughput under `GGML_OPENMP=OFF` has **not** been re-benchmarked (NOT RUN); it must be
  measured before this preset could be promoted.

**Not done / NOT RUN:** the clean-machine test (no machine without the VC++ redistributable is available here); a
`/MT` **Vulkan** build and its real llama/SD1.5 runs (the static preset is CPU-only today); packaging of the static
build (`scripts/package.ps1` still hard-codes the dynamic-CRT description); re-benchmark of `GGML_OPENMP=OFF`. The
single-EXE claim is therefore **NOT** made yet.

## Migration
Add the preset, rebuild everything in a fresh build directory (a CRT change needs a fresh cache), run the gates above.

## Wave B update (2026-11-20): static Vulkan build, status of the single-EXE claim

* New preset `windows-msvc-release-static-vulkan` (`/MT`, `GGML_OPENMP=OFF`, `HCS_ENABLE_VULKAN=ON`) configures and builds
  (666 steps) with MSVC 19.44 / Vulkan SDK 1.4.357.0 without any link or CRT conflict (`docs/test-evidence/wave-b/`).
* `dumpbin /dependents`: `HCSInfer.exe` (CPU) imports ADVAPI32, WS2_32, KERNEL32; `HCSInfer-vulkan.exe` additionally imports
  `vulkan-1.dll` (GGML links the loader's import library). This is why the project ships **two** executables: a program
  importing `vulkan-1.dll` cannot start on a PC without a GPU driver. A delay-loaded loader (one file for both cases) was
  not attempted: ggml's registry would still call Vulkan during start-up and the missing-loader path cannot be tested on
  this machine.
* Restricted-environment runs (empty directory, empty PATH, `scripts/restricted-env-check.ps1`): both EXEs PASS `--help`,
  `doctor --json`, a real LFM3-Omni-2B Q8_0 generation (CPU; Radeon 680M for the Vulkan EXE, 25 layers offloaded), `serve`
  health 200, the GUI page 200, and every module the live server loaded lives under the Windows directory.
* `GGML_OPENMP=OFF` costs about 2% decode / 4% prefill on this CPU in a single A/B run (`docs/benchmarks.md`).
* Verdict (corrected in the release-candidate audit): **no "single-file" or "single executable" claim is made for either
  EXE yet.** There are two static EXEs: the CPU `HCSInfer.exe` imports Windows system DLLs only (`dumpbin`), and
  `HCSInfer-vulkan.exe` additionally imports the system `vulkan-1.dll` that the GPU driver installs. "Single-file" may be
  used only after the clean-machine test (fresh Windows, no redistributable, no SDK) has been run; it is NOT RUN, and both
  binaries are unsigned. The restricted-environment run (empty PATH) is a proxy, not that test.
  Full CTest was not re-run on the new presets (no C++ change); the static Vulkan preset's tests were never built.
