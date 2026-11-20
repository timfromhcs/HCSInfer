# Wave B evidence (distribution), 2026-11-20

Machine: Ryzen 7 7735HS, Radeon 680M, Windows 11 build 26200, MSVC 19.44, CMake 4.4, Vulkan SDK 1.4.357.0 (build only),
Python 3.13 venv (hatchling 1.33.0, build 1.6.1, twine 7.0.0, mypy 2.4.1, pytest 9.2.0), Node 22, actionlint 1.7.7.
Model for every real run: LFM3-Omni-2B Q8_0 (not in the repository).

| Claim | Evidence | Result |
|---|---|---|
| `windows-msvc-release-static-vulkan` configures and builds | `configure-static-vulkan.log`, `build-static-vulkan-tail.log` | PASS (666 steps, exit 0; only upstream compiler warnings) |
| CPU EXE imports system DLLs only | `dumpbin-static-cpu.txt` | PASS (ADVAPI32, WS2_32, KERNEL32) |
| Vulkan EXE imports | `dumpbin-static-vulkan.txt` | adds `vulkan-1.dll` (system loader) |
| Restricted environment (empty dir, empty PATH): help, doctor, real generation, serve health, GUI page, loaded modules | `restricted-env-static-cpu.json`, `restricted-env-static-vulkan.json` | PASS both (Vulkan: 25 layers on the Radeon 680M) |
| Clean machine | `windows-sandbox-check.txt` | NOT RUN (Windows Sandbox not installed; feature query needs elevation) |
| Benchmarks (single runs) | `bench-*.json`, `bench-*.stdout.txt`; summary in `docs/benchmarks.md` | PASS (records), not a controlled A/B |
| Python: unit + real-exe + real-model tests | `pytest-real.txt` | 8 passed |
| Python: `mypy --strict` | `mypy.txt` | no issues in 8 files |
| Wheel/sdist build, `twine check`, fresh venv install, real chat via `Client` | `release-local.txt`, `twine-check.txt`, `wheel-fresh-venv-client.txt`, `sdist-fresh-venv.txt` | PASS (sdist without exe exits 127 with a clear message; with HCSINFER_EXE works) |
| Release files + checksums + SBOM | `SHA256SUMS.txt`, `portable-manifest.json`, `release-local.txt` | produced locally; nothing uploaded |
| Web dist builds and boots | `web-dist-boot.txt`, `web-npm-test.txt` | 13/13 headless Chrome, `npm test` 46/46 |
| Workflow files | `actionlint.txt` | clean; workflows never executed |

NOT RUN: clean-machine test, signing, hosted run of `release.yml`, full CTest on the two new static presets (no C++ source
change in this wave), MSIX/installer (no tool present), PyPI/GitHub/Vercel (owner-side).
