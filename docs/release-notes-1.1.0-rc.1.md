# HCSInfer 1.1.0-rc.1 (release candidate)

Status: **release candidate; tag `v1.1.0-rc.1` pushed, GitHub release is a draft (publishing is the owner's step)**.
Merge commit `e958241` on `main`. The authoritative gate table is `docs/roadmap/STATUS.md`; every statement below is
backed by a file under `docs/test-evidence/`. Items without evidence are `NOT RUN` or `BLOCKED`, never `PASS`.

Verified machine: Ryzen 7 7735HS, Radeon 680M (Vulkan 1.4.315, driver 26.6.1 / Windows AMD package
32.0.21043.12001), ~21.2 GB shared RAM, Windows 11 build 10.0.26200.9974, MSVC 19.44.35228, CMake 4.4.0. No other
machine, GPU or OS was tested.

## What changed since 1.0.0-rc.1

The 41 locally-unpushed wave A/B/C commits were integrated and verified (S0), then their features were exercised with
real files (S1), real-model E2E and controlled benchmarks were recorded (S2), and hardening ran (S3). Highlights:

- Verified image quality envelope + `allow_low_quality` opt-in, `tools/imgqa` metrics and a real 512px gallery.
- LoRA library with compatibility checks, tiled Real-ESRGAN-class 4K upscale through optional ONNX Runtime, and a
  streaming safetensors/GGUF merge command (float tensors only).
- Overload admission controller, telemetry probe, kill switch/watchdog and a stress harness with abort conditions.
- Model Vault prefetch stage (opt-in `serve --vault-prefetch`), models load/unload/status as HTTP clients, faster scan.
- HCSUDA port: Vulkan capability probe, conservative planner, CPU oracle, 8 Vulkan kernels, real `tune` command; no
  kernel is default-enabled because no upstream GGML Vulkan operator baseline was measured.
- Build/analysis scripts repaired for Windows PowerShell 5.1, and 7 clang-tidy findings in the new code fixed.

## Verification (S0-S2, final package in S4)

| Gate | Result | Evidence |
|---|---|---|
| CPU build + full CTest with real models | PASS, 1054/1054, 0 failed, 9 skipped (Vulkan-only/ORT/XSS at the time) | `test-evidence/final-s0/build-cpu-verify.txt`, `ctest-cpu.txt` |
| Vulkan build + full CTest with real models | PASS, 1085/1085, 0 failed, 4 skipped | `test-evidence/final-s0/build-vulkan.txt`, `ctest-vulkan.txt` |
| clang-format / clang-tidy | PASS, 298 files / 0 diagnostics in 92 TUs | `test-evidence/final-s0/format-after-fixes.txt`, `clang-tidy-FINAL.txt` |
| web `npm test` / Python pytest+mypy | 46/46; 5 passed + 4 skipped (real EXE not yet built), mypy 0 issues | `test-evidence/final-s0/web-npm-test.txt`, `python-mypy.txt` |
| Real LCM-LoRA render | PASS, applied id/weight reported and PSNR 11.50 dB / SSIM 0.64 vs baseline; both images viewed | `test-evidence/final-s1/` |
| Real ONNX 4K upscale | PASS, 512x512 -> 3840x3840, 4 tiles, 73.3 s, image viewed | `test-evidence/final-s1/upscale-real.txt` |
| B0/B1 full benchmarks, same LFM3 hash/seed | PASS, Vulkan 1.90x decode and 8.27x TTFT vs CPU | `test-evidence/final-s2/bench-{cpu,vulkan}-full.json` |
| HCS Studio on Vulkan with real models | PASS, 42/42 checks | `test-evidence/final-s2/studio-shots-vk.txt` |
| Full soak profile + fault injection | PASS, 13/13 phases incl. 30.1 min generation and 18/18 HttpStorm checks | `test-evidence/final-s3/soak-full/summary.json` |
| 30-min drift / leak | PASS, +0.63% decode drift, 41.0 MiB private growth over 100 cycles, handle growth 0 | `test-evidence/final-s3/soak-full/soak-core.jsonl` |
| Parser fuzz smoke / XSS | PASS: 376k executions, 0 crashes (UBSan, no ASan) / 9/9 XSS checks | `test-evidence/final-s3/libfuzzer-smoke.txt`, `xss/xss-run.txt` |
| Static CPU + Vulkan builds, restricted-env proxy | PASS: 515/515 and 876/876 build steps; real generation with an empty PATH; 0 modules outside `C:\Windows` | `test-evidence/final-s4/{build-static-*,restricted-static-*}` |
| Release set, SHA-256, web/wheel smoke | PASS: 2 EXEs, portable ZIP, wheel + sdist (`twine check`), web dist, SBOM, 7/7 hashes; web 46/46 | `test-evidence/final-s4/release-local.txt`, `release-verify.txt`, `web-npm-test.txt` |

## Known limits / blockers

- A real merge of two distinct float models is **BLOCKED** (no same-architecture float pair locally; quantized GGUF is
  refused by design). No merged model was loaded into an engine.
- No default-enabled HCS Vulkan kernel: all kernels are correct and measured, but the upstream GGML Vulkan operator
  baseline is `NOT RUN`, so the planner keeps the proven CPU/upstream path.
- Real ONNX runtime uses the CPU provider only (this build has no DirectML export); no GPU upscale claim.
- Real trainer run, real `prime-agent` session, real WebGPU inference, Vercel deploy and a clean-machine run are NOT
  RUN. A physical USB cable pull and true RAM exhaustion are replaced by documented fault injection.
- Long scheduled fuzzing and ASan are NOT RUN (toolchain CRT mismatch); a 61-second UBSan/libFuzzer smoke passed.
- One of three final Vulkan full-CTest runs had a transient failure in `VulkanRunner.DispatchTimeoutMarksTheRunnerLostAndResetRecovers`
  (`prepare_gemm_f32` returned an error once); the test passed in isolation, in the 32-test kernel group, and in the two
  subsequent full runs (both 1085/1085). Root cause was not reproduced; classified as transient device/driver state and
  kept visible as a residual risk. Full red log: `test-evidence/final-s5/ctest-static-vulkan.txt`.
- Code signing, publishing the GitHub draft and PyPI upload require the owner's credentials and are not done here.

No model weights, prompts, images of users or tokens are shipped in the repository or the package.
