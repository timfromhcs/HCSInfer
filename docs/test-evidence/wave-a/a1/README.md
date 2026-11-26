# Wave A / a1 - "nothing on C:" storage guard

Machine: Windows 11 10.0.26200, MSVC 19.44.35228, CMake 4.4.0 (Python wheel), Ninja, clang-format 19.1.5.
Branch `wave-a/a1`, preset `windows-msvc-debug` (build dir inside the worktree, on D:). The worktree has no initialised
`external/llama.cpp` / `external/stable-diffusion.cpp`, so the engine modules were skipped.
The network clone of Vulkan-Headers stalled (several agents configuring at once), so configure used
`-DFETCHCONTENT_SOURCE_DIR_*` pointing at the already-fetched sources under `build/windows-msvc-release/_deps` (read only).

| Item | Result | Evidence |
|---|---|---|
| `StateDir.*` unit tests, red first | red: `LNK2019 unresolved external symbol hcs::resolve_state_dir`; green: 5/5 PASS | red-build-excerpt.txt, gtest-statedir.log |
| CTest subset `-L "guard\|unit\|contract"` (761 tests) | 758 PASS, 3 FAIL (see below) | ctest-subset-summary.txt |
| `guard_no_c_writes_doctor` (label `guard`) | PASS (re-run after the robocopy rewrite, 96 s) | guard-positive.json, ctest-guard2.log |
| Guard negative self-test (60 MB written into a watched dir on D:) | FAIL as intended, exit 1, growth 62914560 B | guard-negative.json |
| `scripts/format.ps1` | clean after `-Fix` on 3 files | - |

Failing tests: `Cli.BenchmarkHelpAndArgumentValidationBeforeLoadingAnything`,
`Cli.ImplementedTransportsHaveHelpAndValidateArguments`, `Cli.ChatValidatesArgumentsBeforeLoadingAnything`. They return
exit code 10 (Unsupported) where 2 is expected; the engine modules are absent in this worktree
(`external/llama.cpp missing - module skipped`). Not re-run on a baseline tree with engines, so "environmental" is the
likely cause, not proven.

## C: free space (informational; system-wide, other agents ran in parallel)
- Start of session: 28 551 446 528 B. End: 59 884 154 880 B (+31.3 GB, other processes freed space; not this task).
- Guard run (`doctor --json`): C: free before 59 448 320 000 B, after 59 588 079 616 B. In the first (slow, 4 min)
  version, summed watched-dir growth was 34.7 MB, entirely `%LOCALAPPDATA%\Temp` traffic from the harness. The rewrite
  excludes the harness's `claude` dir; the final guard run passed (guard-positive.json).
- `%LOCALAPPDATA%\Temp` held ~75 GB when measured; `%LOCALAPPDATA%\HCSInfer` is 1098 B.

## Leftover HCS-related items on C: (listed, NOT deleted)
See c_leftovers.txt: about 5 GB of `hcs-studio-xss-*`, `hcs-studio-shots-*`, `hcs-gguf-*`, `hcs-utxo-*`, `hcs-core-*`
folders in `%LOCALAPPDATA%\Temp` from earlier test runs. They predate this guard.
