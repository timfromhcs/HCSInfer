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
- Pursue option 2 as an experiment on a dedicated preset (`windows-msvc-release-static`, not yet created) because it is
  the only way to a one-EXE release. Promote it only when all of the following have passed: the full test suite with
  `/MT`; real llama and SD1.5 runs on CPU and Vulkan; the binary starts on a Windows install without the VC++
  redistributable and without the Vulkan SDK (clean-machine test); `dumpbin /dependents` shows only system DLLs;
  licence notices complete.
- Do not label any release "single-file" before those results exist.

## Rejected
Setting `/MT` globally by editing upstream CMake files; forcing it in a way that breaks upstream's own options.

## Risks
Upstream may link something that is incompatible with `/MT` (OpenMP, prebuilt third-party libs); `/MT` increases binary
size; the static CRT and thread/locale behaviour must be retested. Vulkan loading (`vulkan-1.dll`) stays dynamic by design.

## Evidence
None for `/MT` (NOT RUN). Today's evidence is only that the `/MD` build works (see `docs/roadmap/STATUS.md`).

## Migration
Add the preset, rebuild everything in a fresh build directory (a CRT change needs a fresh cache), run the gates above.
