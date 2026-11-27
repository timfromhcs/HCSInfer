# S5 evidence: final gate for 1.1.0-rc.1 (2026-11-27)

Branch `release/v1.1-stable`, clean tree at `e81d002`; package rebuilt from that commit. One real-model activity at a
time with `realmodel.lock`; caches/temp on D:.

| Gate | Status | Evidence |
|---|---|---|
| Static CPU build from final tree | PASS | `build-static-cpu.txt` 515/515 steps; reconfigure `reconfigure-static-cpu.txt` so the EXE embeds the final commit. |
| Static Vulkan build from final tree | PASS | `build-static-vulkan.txt` 876/876 steps; 10 shaders compiled, `spirv-val`-validated and embedded; reconfigure `reconfigure-static-vulkan.txt`. |
| Full CPU CTest (static preset, real models) | PASS | `ctest-static-cpu.txt`: 1054/1054, 0 failed, 8 environment skips; 1555.77 s; free RAM logged. |
| Full Vulkan CTest (static preset, real models) | PASS with a recorded transient | 3 runs: `ctest-static-vulkan.txt` 1084/1085 (one `VulkanRunner.DispatchTimeout...` prepare failure), `ctest-static-vulkan-rerun.txt` and `ctest-static-vulkan-run3.txt` **1085/1085 each**. Repro attempts: isolated test SKIP, `-L kernel-vulkan` 32/32, `-I 1000,1025` 26/26 (`ctest-vk-timeout-RED.txt`, `ctest-vk-kernels-RED.txt`, `ctest-vk-range-1000-1025.txt`). The assertion now prints `op.error().message` (diagnostic only). Root cause not reproduced; treated as transient device/driver state and listed as residual risk, not silently dropped. |
| Format / clang-tidy / web | PASS | `format.txt` 298 files, `clang-tidy.txt` 0 diagnostics in 92 TUs, `web-npm-test.txt` 46/46. |
| Release set rebuilt from `e81d002` | PASS | `release-local-final.txt`: CPU EXE `e440172c...2362cf`, Vulkan EXE `b639bd8e...5cc7`, ZIP `c156cf85...3d00`, wheel/sdist (`twine check` PASS), web dist, SBOM 15 components. |
| SHA-256 / wheel / restricted-env | PASS | `release-verify-final.txt` 7/7 verified; `wheel-smoke-final.txt` `HCSInfer 1.1.0-rc.1`; `restricted-final-{cpu,vulkan}.json` health 200, GUI 200, 0 modules outside `C:\Windows` with real CPU/Vulkan generation. |

## Release decision

`1.1.0-rc.1`, not `1.1.0`: real distinct-model merge (no float pair), real trainer/prime-agent runs, clean-machine VM,
signing, long fuzz/ASan, Vercel and publishing remain BLOCKED/NOT RUN. The version string, release notes and STATUS
agree. Publishing the GitHub draft is the owner's step.
