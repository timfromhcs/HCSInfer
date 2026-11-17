# Static analysis (clang-tidy) - first run, triage and baseline

Status: **gate is clean with its documented default check set**; the full check set is a triaged baseline.
Tool: `clang-tidy`, run by `scripts/static-analysis.ps1` over the HCS-owned translation units of `build/img-api`
(Release, CPU build, `/W4 /WX`, MSVC 19.44.35228, compile database from CMake). Locally that is the
Visual-Studio-2022-bundled **19.1.5**; on the CI runner the script falls back to the LLVM install, **20.1.8** - both
were run and both are clean (see "The same gate on the CI runner").

```
.\scripts\static-analysis.ps1 -Preset windows-msvc-release -Scope src -Parallel 4
```

The script loads the Visual Studio 2022 environment before analysing: clang resolves the MSVC STL through the
registry and would otherwise pick the newest Visual Studio on this machine (a VS 18 BuildTools whose STL demands
Clang 20+ and fails with `STL1000`). `/WX` from the HCS targets is neutralised with `-Wno-error` so the analysis
always completes.

## Result

| Run | Checks | Diagnostics in HCS code |
|---|---|---|
| first ever run | `-*,clang-analyzer-*,bugprone-*` | **81** in 42 translation units - `test-evidence/static-analysis/clang-tidy01-first-run-81.txt` |
| after the fixes below | `-*,clang-analyzer-*,bugprone-*` | **36** in 42 translation units, all in the triaged classes below - `.../clang-tidy03-all-checks-after-fixes-36.txt` |
| default gate | the four exclusions below | **0** in 42 translation units - `.../clang-tidy02-default-checks-clean.txt` |

The full test suite was re-run after the code changes: **400 tests, 396 passed, 0 failed, 4 skipped (Vulkan)**, 267 s
(`.../ctest-after-fixes-400-green.txt`), and both the CPU and the Vulkan build compile and link.

### The same gate on the CI runner

The `clang-tidy (static analysis)` job of `.github/workflows/pr.yml` ran on 2026-11-17 in run 44114113997 and
reported **`STATIC ANALYSIS OK: no diagnostic in 47 translation units`**. Worth recording: on the runner
`Find-VsLlvmTool` fell back to the PATH/LLVM install and used **clang-tidy 20.1.8**
(`C:\Program Files\LLVM\bin\clang-tidy.exe`), not the VS 2022-bundled 19.1.5 - i.e. two different clang-tidy
versions, over 47 rather than 42 translation units (a fresh configure lists a few more), both clean with the same
documented check set. The job is non-blocking for one cycle until that has been observed on the runner's own terms;
the version used, the translation-unit count and the diagnostic count are all printed, so a version drift cannot
hide.

## Fixed (real, or clearly worth having)

| Finding | Site(s) | Change |
|---|---|---|
| `bugprone-misplaced-widening-cast` (3) | `src/api/data_uri.cpp` | the cast happens before the arithmetic (`t[static_cast<std::size_t>('A') + i]`) |
| `bugprone-implicit-widening-of-multiplication-result` (12) | `include/hcs/api/http_api.hpp`, `include/hcs/api/control_plane.hpp`, `include/hcs/mcp/mcp_server.hpp`, `src/api/runtime.cpp`, `src/engines/{llama,diffusion}` | size constants widened first (`std::size_t{1024} * 1024`) |
| `bugprone-implicit-widening-of-multiplication-result` (4) | `src/storage/sha256.cpp` | loop counters and index arithmetic are `std::size_t`; the SHA-256 test vectors still pass |
| `clang-analyzer-deadcode.DeadStores` (1) | `src/hardware/vulkan_info.cpp` | the last `tail = &...pNext` was genuinely dead |
| `bugprone-branch-clone` (2) | `src/api/http_api.cpp`, `src/engines/diffusion/diffusion_engine.cpp` | identical switch arms merged into one group (behaviour unchanged) |
| `bugprone-unchecked-optional-access` (3) | `src/cli/vision_cmd.cpp`, `src/core/job_queue.cpp` | the optional is bound to a reference inside its guard |
| `bugprone-empty-catch` (3) | `src/api/control_plane.cpp`, `src/core/job_queue.cpp`, `src/mcp/mcp_server.cpp` | the deliberately swallowed exceptions now carry an explicit no-op body plus the reason |
| `bugprone-not-null-terminated-result` (1) | `src/storage/hcsmodel.cpp` | suppressed in place with the reason: the preceding `memset` guarantees the terminator |
| `bugprone-exception-escape` (4) | `src/core/job_queue.cpp` (`~JobQueue`), `src/app/main.cpp`, `src/storage/codec.cpp` | a destructor cannot let `std::system_error` escape; `main` reports an escaping exception instead of `std::terminate` with no message; the `noexcept` version accessors can no longer die on `bad_alloc` |
| `clang-analyzer-deadcode.DeadStores` (1) | `src/storage/writer.cpp` | **false positive, kept and annotated**: the store is read by the `Cleanup` RAII guard's destructor |

## Triaged: not defects here (the four default exclusions)

| Check | Hits | Why it is not a defect in this code base |
|---|---|---|
| `bugprone-easily-swappable-parameters` | 22 | Every hit is a documented positional parameter of a narrow API (`state_dir`, `preset`, `path, root`, ...). Silencing it means changing public signatures to add strong types purely for the analyser. |
| `bugprone-casting-through-void` | 4 | All four are the required loader idiom: `GetProcAddress`/`LoadLibrary` (`src/hardware/gpu_info.cpp`, `system_info.cpp`, `vulkan_info.cpp`) and `vkGetInstanceProcAddr`, which the Vulkan specification itself defines as returning a generic function pointer. |
| `bugprone-exception-escape` | 6 (after 4 real fixes) | `HttpRequest`'s implicit constructor (allocating members, like every such aggregate) and four job-body lambdas. Job bodies run inside `JobQueue::worker_loop`, which wraps every call in `catch (const std::exception&)` / `catch (...)` and turns a throw into a failed job - that is exactly where a throw must be caught, so nothing escapes. |
| `clang-analyzer-optin.core.EnumCastOutOfRange` | 4 | The diagnostic is inside the **MSVC STL header** `xfilesystem_abi.h` (the standard library's own `__std_fs_stats_flags` cast), not in HCS-owned code. |

The exclusions live in the script's default `-Checks` value with the reason inline; `-Checks` overrides it
completely, so the full set is one flag away and nothing is hidden.

## Not covered yet

* `tests/` and `tools/` are **report-only**: `.\scripts\static-analysis.ps1 -Scope all` finds 31 diagnostics there
  (baseline, measured 2026-11-17 over 74 translation units) and the CI job runs `-Scope src` only. The classes:
  `bugprone-unchecked-optional-access` (25) is the `ASSERT_TRUE(opt.has_value()); ... opt->x` idiom - gtest's `ASSERT_*`
  returns from the test on failure, so the access is guarded and the check cannot see that;
  `bugprone-implicit-widening-of-multiplication-result` (6) are constant
  `1 << 30`-sized buffer offsets in test fixtures and one `16 * i` in `test_stream.cpp`. All are test/Tool code that
  passes; none is a defect in shipped logic. Three real findings in `tools/benchmark-runner/storage_bench.cpp`
  (two `bugprone-narrowing-conversions` on the `__cpuid` leaves and one pointer-offset widening) were fixed.
* No `performance-*`, `modernize-*`, `portability-*` or `readability-*` checks; the goal so far was defects, not style.
* Only the CPU Release preset is analysed. It still compiles `src/hardware/vulkan_info.cpp` (`HCS_ENABLE_VULKAN=OFF`
  guards the runtime paths, not the file), which is where one finding was fixed.
* On the CI runner the analysis currently runs with **clang-tidy 20.1.8** (the LLVM install found via PATH), not the
  VS 2022-bundled 19.1.5 the script prefers locally: both report 0 diagnostics with the same check set, so the gate is
  version-robust here, but the version is worth pinning if a future finding differs between them.
