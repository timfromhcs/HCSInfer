# Evidence: W4 static-CRT single-EXE experiment (ADR-008)

Status: **PARTIAL** — the static CRT build works, is system-DLL-only and passes the full suite, but the ADR-008
promotion gate is not met (the clean-machine test is NOT RUN, and there is no `/MT` Vulkan build). The single-EXE
claim is **not** made.

## Configuration

* Preset `windows-msvc-release-static`: `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` (`/MT` for **every** target,
  including upstream engines) + `GGML_OPENMP=OFF`. Build dir `build/windows-msvc-release-static`.
* Toolchain: MSVC 19.44.35228 (VS 2022 Community), CMake 4.4.0, Ninja 1.13.2.
* Source: working tree at `af884c3` + the W4 preset change (see the commit that adds this directory).
* Date: 2026-11-18.

## Results

| Check | Result | Evidence |
|---|---|---|
| Fresh configure + build (448 targets) | PASS | `build\windows-msvc-release-static` |
| Full Release CTest | **PASS 783/783, 0 failed** (CPU-only; model/Vulkan tests skip = NOT RUN) | `ctest-summary.txt` |
| `dumpbin /dependents` shows only system DLLs | **PASS** | `dumpbin-static.txt` |
| Real GGUF text generation (CPU) | PASS — "HCSInfer static build works.", load 825 ms, TTFT 503 ms, 16.4 t/s | `real-run-static-chat.json` |
| Real SD1.5 Q8_0 render (CPU, 256x256, 2 steps) | PASS — 113 696-byte PNG, 176 s | reproduced, not archived as a file here |
| `/MT` regression vs `/MD` (warm file cache, same model/prompt) | none visible | see the comparison below |

### `dumpbin /dependents` (the point of W4)

* Static experiment: `ADVAPI32.dll`, `WS2_32.dll`, `KERNEL32.dll` — **only Windows system DLLs**.
* Default `/MD` build: additionally `MSVCP140.dll`, `VCRUNTIME140*.dll`, `VCOMP140.DLL`, `api-ms-win-crt-*.dll`,
  i.e. it needs the Visual C++ Redistributable and links MSVC OpenMP (`vcomp140`) dynamically. Turning OpenMP off
  (`GGML_OPENMP=OFF`; ggml then uses its own thread pool) is what removes the last non-system dependency.

### CRT comparison (controlled, same state dir, file in the OS cache)

`HCSInfer mcp stdio` → `hcs.runtime.benchmark`, model `lfm3-omni-2b-q8_0` (LFM3-Omni-2B Q8_0), quick suite:

| Build | cold load (ms) | TTFT (ms) | decode (t/s) |
|---|---|---|---|
| `/MD` + OpenMP | 1225.6 | 2096.4 | 19.30 |
| `/MT` + no OpenMP | 909.5 | 2470.8 | 19.01 |
| `/MD` + OpenMP (repeat) | 896.8 | 2591.1 | 19.15 |

Single samples each; differences are within run-to-run noise. A first-touch cold-disk load was measured at ~77–80 s
once (file not cached), which is a **storage/cache** effect, not a CRT effect: with the file cached the same build
reports ~0.9 s. On a cold cache the MCP benchmark can even exceed the 300 s MCP call timeout and be cancelled — this
is the documented storage dependency (AGENTS.md 11), not a W4 defect.

## NOT RUN / BLOCKED

* **Clean-machine test** (start the EXE on a Windows install without the VC++ redistributable and without the Vulkan
  SDK): NOT RUN — no such machine is available here. This is the gate that decides promotion.
* `/MT` **Vulkan** build and its real llama/SD1.5 Vulkan runs: NOT RUN (the static preset is CPU-only today).
* **Packaging** the static build: NOT RUN (`scripts/package.ps1` still writes the dynamic-CRT description).
* A full `HCSInfer benchmark --suite full` on the static build: NOT RUN.

## Reproduce

```powershell
.\scripts\build.ps1 -Preset windows-msvc-release-static
ctest --test-dir build\windows-msvc-release-static --output-on-failure
# in an MSVC x64 environment:
dumpbin /dependents build\windows-msvc-release-static\bin\HCSInfer.exe
```
