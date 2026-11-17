# M10 audit fixes: evidence

Build: `build/audit-fix`, Release, CPU-only (`HCS_ENABLE_VULKAN=OFF`), MSVC 19.44 x64, Ninja, llama.cpp engine linked,
`/W4 /WX` on HCS targets. Vulkan paths are NOT RUN here.

Commands (from `D:\HCSInfer`, MSVC environment via `scripts\common.ps1` `Initialize-MsvcEnvironment`):

```
cmake --preset windows-msvc-release -B build/audit-fix
cmake --build build/audit-fix --parallel 4 -- -k 0
# RED (before the fixes, same tests, unfixed sources) and GREEN (after):
build\audit-fix\bin\hcs_test_core.exe     --gtest_filter=PathGuard.*:Settings.*
build\audit-fix\bin\hcs_test_mcp.exe      --gtest_filter=McpAudit.*
build\audit-fix\bin\hcs_test_api.exe      --gtest_filter=ApiAudit.*
build\audit-fix\bin\hcs_test_api_http.exe --gtest_filter=ApiSocketAudit.*
build\audit-fix\bin\hcs_test_hardware.exe --gtest_filter=MemoryProbe.*
# Full suite with the real models:
set HCS_TEST_GGUF=E:\AILOCALTRAIN\data\gguf\qwen3-0.6b-q4_k_m.gguf
set HCS_TEST_VLM=D:\HCSModels\vision\SmolVLM2-500M-Video-Instruct-Q8_0.gguf
set HCS_TEST_MMPROJ=D:\HCSModels\vision\mmproj-SmolVLM2-500M-Video-Instruct-Q8_0.gguf
ctest --test-dir build/audit-fix --output-on-failure --timeout 1800
```

| File | Content |
|---|---|
| `red-core.txt`, `red-mcp.txt`, `red-api*.txt`, `red-socket*.txt` | the new tests against the UNFIXED sources (failures, crashes, hangs; the notes at the end of a file explain exit codes 127/124) |
| `green-*.txt` | the same tests after the fixes (all pass) |
| `repro-binary-before.txt` | black-box runs of the previously built `build/windows-msvc-release/bin/HCSInfer.exe` (D1 crash, D2 starvation, D3 runaway job) |
| `repro-binary-after.txt` | the same runs on `build/audit-fix/bin/HCSInfer.exe`, plus D4 on the real qwen3-0.6b |
| `ctest-release-full.txt` | full Release suite at that point: 368 tests, 363 passed, 5 skipped (4 Vulkan = NOT RUN on a CPU build, `e2e_gui_real` = other owner), 0 failed |
| `ctest-full-400-green.txt` | **final** full Release suite after the image routes, the GUI hook and the benchmark command landed: **400 tests, 396 passed, 4 skipped (Vulkan, NOT RUN on a CPU build), 0 failed** (223 s) |
| `e2e-api-mcp-real-after-fix.json`, `e2e-vision-real-after-fix.json` | real-model end-to-end results (qwen3-0.6b Q4_K_M; SmolVLM2-500M Q8_0 + mmproj), CPU |

Defects found after this pass while building the final suite, both in test code, both fixed:

* `tests/e2e/image_real/e2e_image.py` had a corrupted multi-line string (Python syntax error), so the image e2e could
  not run at all; it is now a normal `"\n".join([...])` and the test passes 47/47 (`../M3-image/`).
* `tests/e2e/api_mcp_real/e2e_real.py` still asserted that `/v1/images/generations` answers `501`; since the route is
  real, it now asserts the honest behaviour (501 without an engine, 409 without an image model) and the full render
  path is covered by `e2e_image_real`.

The red-run tests for D3 hung (exit 124) because the runaway generation never ended in the unfixed build: that is the
defect, not a harness problem. The two `DeeplyNested*` MCP tests killed the test process (stack overflow, exit 127 in
git-bash, `0xC00000FD` natively).
