# CI evidence

## clang-format gate (`scripts/format.ps1`, job `format` in `.github/workflows/pr.yml`)

Date: 2026-11-17. `format-check.txt`: **FORMAT CHECK OK: 139 files**, exit 0.

Before this pass the tree had never been formatted: the check reported 113 of 139 files differing, which is why the
workflow job carries `continue-on-error: true` (documented in the job comment since it was added).

### Which clang-format, and why it matters

`clang-format` output is version-dependent: two versions can wrap the same code differently, so a formatting pass is
only meaningful if local and CI use the same formatter. This machine has three:

| Location | Version |
|---|---|
| `C:\Program Files\AMD\ROCm\7.1\bin\clang-format.exe` (first on PATH) | 21.0.0git |
| `C:\Program Files\LLVM\bin\clang-format.exe` | 22.1.8 |
| `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe` | **19.1.5** |

The GitHub `windows-latest` runner (Windows Server 2025 image, `20261111.331.1`) ships **LLVM 20.1.8** and Visual
Studio 2022 Enterprise with the `Microsoft.VisualStudio.Component.VC.Llvm.Clang` component. `scripts/format.ps1` was
therefore changed to prefer the **Visual-Studio-bundled** clang-format (19.1.5 here) over whatever happens to be first
on PATH, so both sides format with the Microsoft-bundled compiler toolchain rather than with a random LLVM on PATH.
The version actually used is printed on every run.

### Consequence found while formatting (and fixed in the same change)

Google style sorts `#include` directives by default. That broke the build: in three files `psapi.h` moved before
`windows.h`, and `psapi.h` requires `windows.h` first (`error C2146: missing ';' before identifier 'WINAPI'` in the
Windows SDK headers). `.clang-format` now sets `SortIncludes: Never` and `IncludeBlocks: Preserve` (with the reason
inline), the three files were put back in the load-bearing order, and the whole tree was rebuilt:

* CPU build `build/img-api`: rebuilt and linked, 0 errors; full suite **400 tests, 396 passed, 0 failed, 4 skipped
  (Vulkan), 226 s** (`../M10-fixes/ctest-postformat-400-green.txt`).
* Vulkan build `windows-msvc-release-vulkan`: recompiled and linked, 0 errors (the Vulkan module includes
  `vulkan/vulkan_core.h` and `windows.h`).

## NOT RUN in CI

No workflow has ever executed: the repository had no remote. The first GitHub Actions run is still pending.
