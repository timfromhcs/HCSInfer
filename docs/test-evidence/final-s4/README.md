# S4 evidence: 1.1.0-rc.1 release package (2026-11-27)

Branch `release/v1.1-stable` from commit `d185678` (version bump + docs in the same wave). All builds are from a clean
configured tree; temp/caches on D: via `scripts/env-d-e.ps1`.

| Gate | Status | Evidence |
|---|---|---|
| Version strings | PASS | CMake `project(VERSION 1.1.0)` + suffix `-rc.1`; both static EXEs and the installed wheel print `HCSInfer 1.1.0-rc.1` (`release-local.txt`, `wheel-smoke.txt`); wheel `1.1.0rc1`, web package `1.1.0-rc.1`, README/install/security/support/CHANGELOG updated. |
| Static CPU `/MT` build | PASS | `build-static-cpu.txt`: BUILD OK, 515/515 steps, no HCS warnings (upstream C4068/D9025 and the deliberate `/W3 /WX-` for the two httplib-facing files are documented). |
| Static Vulkan `/MT` build | PASS | `build-static-vulkan.txt`: BUILD OK, 876/876 steps; all 10 shaders compiled with `glslc` + `spirv-val` and embedded. |
| Restricted-environment proxy (both EXEs, real model) | PASS | `restricted-static-{cpu,vulkan}.json` + `.txt`: copied alone into a fresh dir with empty PATH; help, doctor JSON, real CPU generation ("static ok") / real Vulkan generation (4.67 s), `serve` health 200 + GUI page 200. CPU: 14 modules, all under `C:\Windows`; Vulkan: 16 modules incl. `vulkan-1.dll`, 0 outside Windows. This is **not** a clean-machine test (NOT RUN). The script had a real PS 5.1 `Kill(bool)` overload defect, fixed red->green. |
| Release set (`scripts/release-local.ps1`) | PASS | `release-local.txt`: static CPU EXE SHA-256 `e1daef64...c160` (45,304,320 B), static Vulkan EXE `885c79d7...1729` (87,839,744 B), portable ZIP `77974fa5...6657`, wheel `hcsinfer-1.1.0rc1`, sdist, web dist (12 files), CycloneDX SBOM (15 components). `twine check` PASSED both Python artifacts. |
| SHA-256 manifest | PASS | `release-verify.txt`: 7/7 entries verified, LF-only line endings. |
| Web tests / wheel smoke | PASS | `web-npm-test.txt` 46/46; `wheel-smoke.txt` fresh venv install + `hcsinfer --version` = 1.1.0-rc.1. |
| Licences | PARTIAL | `prime-agent/LICENSE` missing because the pinned submodule is not initialised in this checkout; the release script reports the warning and the SBOM notes it. No other licence gap found. |

## Not claimed

No single-file EXE (two static EXEs, the Vulkan one imports the system `vulkan-1.dll`), no signed build, no clean VM,
no published GitHub release / PyPI upload. Final full-CTest re-run on this exact tree is the S5 gate.
