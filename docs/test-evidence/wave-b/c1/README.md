# Wave B / c1 evidence (HCSUDA port, Radeon 680M)

Machine: AMD Ryzen 7 7735HS, AMD Radeon(TM) Graphics (680M), driver 26.6.1 (raw 0x00800161), Vulkan 1.4.315,
subgroup 64, Windows 11. Build: `windows-msvc-release-kernels` (Release, MSVC 19.44), build dir `D:\HCSInfer\build\c1`.

| file | what |
|---|---|
| `vulkan_ops_run.txt` | full gtest output of `hcs_test_vulkan_ops` (per-case max abs error vs tolerance, validation layer active) |
| `tune-evidence.json` / `tune-stdout.txt` | `HCSInfer tune --suite full`: per kernel and bucket median/spread, GFLOP/s, oracle error, CPU baseline ratio, device/driver metadata |
| `tune-validation-quick.json` / `tune-validation-stdout.txt` | `tune --suite quick --validation`: Khronos validation layer, 0 warning/error messages |
| `tune-show.txt`, `doctor.json`, `doctor-with-profile.json` | planner table; `doctor --json` (planner section) without and with the saved profile |
| `ctest-*.txt` | ctest logs |

Not run: upstream GGML Vulkan operator baseline (NOT RUN), so no kernel is default-enabled.

Full ctest (serial, engines built, kernels ON): 1086 tests, 100% passed, 42 skipped (real-model tests skipped: no model env configured, so real-model gates are NOT RUN in this run). Log tail: `ctest-full-summary.txt`.
