# HCSInfer 1.0.0-rc.1 (release candidate)

Status: **release candidate, not tagged, not merged, not pushed.** This is an alpha-quality, single-machine-verified
build. Every statement below is backed by a file under `docs/test-evidence/` or an inspected source file; anything
without evidence is listed as NOT RUN or BLOCKED. The authoritative per-gate table is `docs/roadmap/STATUS.md`; the
tested hardware/model list is `docs/support-matrix.md`.

Verified machine: Ryzen 7 7735HS, Radeon 680M (AMD driver 32.0.21043.12001, Vulkan 1.4.315), ~21.2 GB RAM, Windows 11
build 10.0.26200.9974, MSVC 19.44.35228, CMake 4.4.0. No other machine, GPU or OS was tested.

## Wave R verification (this release candidate)

| Gate | Result | Evidence |
|---|---|---|
| clang-format (19.1.5, 221 files) | PASS (4 kernel files were unformatted after wave K; reformatted) | `docs/test-evidence/wave-r/format-red-before.txt`, `format-check.txt` |
| clang-tidy `-Scope src -FailOnFindings` | PASS, 0 diagnostics in 69 translation units (the feature-flagged Vulkan kernel TU is not in the Release compile database) | `docs/test-evidence/wave-r/clang-tidy.txt` |
| Full CTest, `windows-msvc-release` (CPU) | see `docs/test-evidence/wave-r/RESULTS.md` | `docs/test-evidence/wave-r/ctest-release-cpu.txt` |
| `kernel-vulkan` CTest, `windows-msvc-release-kernels` | see `docs/test-evidence/wave-r/RESULTS.md` | `docs/test-evidence/wave-r/ctest-kernels.txt` |
| `web/` `npm test` | see `docs/test-evidence/wave-r/RESULTS.md` | `docs/test-evidence/wave-r/npm-test.txt` |
| Portable package + ZIP + SHA256SUMS + SBOM | see `docs/test-evidence/wave-r/RESULTS.md` | `docs/test-evidence/wave-r/` |

## Wave B: distribution (2026-11-20)

| Item | Result | Evidence |
|---|---|---|
| Static CPU `HCSInfer.exe` (44,229,120 bytes, sha256 `c3a613e6...`) | imports ADVAPI32, WS2_32, KERNEL32 only; restricted-env run PASS incl. real LFM3-Omni-2B generation ("static ok"), `serve` health and GUI page 200 | `docs/test-evidence/wave-b/restricted-env-static-cpu.json`, `dumpbin-*.txt` |
| Static Vulkan `HCSInfer-vulkan.exe` (86,548,992 bytes, sha256 `b94a6e06...`) | builds; imports the same plus `vulkan-1.dll`; restricted-env run PASS incl. real generation on the Radeon 680M (25 layers offloaded) | `restricted-env-static-vulkan.json`, `bench-vulkan-static-vulkan.json` |
| Python wheel `py3-none-win_amd64` + sdist | `twine check` PASSED, fresh-venv install, `hcsinfer --help`, `Client`/`Server` real chat + streaming (CPU exe and Vulkan exe), pytest 8/8, `mypy --strict` clean | `pytest-real.txt`, `wheel-fresh-venv-client.txt`, `sdist-fresh-venv.txt`, `twine-check.txt`, `mypy.txt` |
| Portable ZIP + `SHA256SUMS.txt` + CycloneDX SBOM (15 components) | produced by `scripts/release-local.ps1` | `release-local.txt`, `SHA256SUMS.txt`, `portable-manifest.json` |
| Web demo dist (`web/dist`, not deployed) | builds; boots in real headless Chrome 13/13; `npm test` 46/46 | `web-dist-boot.txt` |
| `release.yml`, `pr.yml` python + dist jobs | actionlint v1.7.7 clean; never executed | `actionlint.txt` |

Details and limits: `docs/packaging.md`, `docs/install.md`.

## What works (evidence-backed)

- **Hardware doctor** (`HCSInfer doctor [--json]`): CPU features, RAM, OS, Vulkan devices/heaps, drive info; unknown
  values print `unknown` (`M0/`).
- **Text chat** through llama.cpp on CPU and on Vulkan (Radeon 680M), including OpenAI-style tool calls with LFM3-Omni-2B
  Q8_0 (`benchmark/`, `lfm3/`). Upstream Vulkan decode: 148.1 t/s (qwen3-0.6b Q4_K_M, CPU 75.1) and 39.7 t/s
  (LFM3-Omni-2B Q8_0, CPU 19.3), 5 repetitions, median (`vulkan-kernels/m8-baseline/`).
- **Vision** (one image per single-turn request) with SmolVLM2-500M and LFM3-Omni-2B, CPU and Vulkan (`M2-vision/`).
- **Image generation and editing**: SD1.5 Q8_0 on CPU and Vulkan (`M3-image/`, 47/47 checks over a real socket on both
  builds) and **FLUX.3 Image Klein** (unsloth UD-Q4_K_XL) through stable-diffusion.cpp, text-to-image and
  image-to-image on Vulkan (CPU: text-to-image engine test only, no CPU image-to-image run), with the substitute Qwen3-VL-4B Q4_K_M text encoder and FLUX.2 VAE (the gated
  official Black Forest Labs files were not accessible). 512x512 takes 364-471 s on the 680M (single samples). The
  `fast` preset is 6 steps, chosen by a measured 4/6/8/12/16 sweep with PSNR/DSSIM against the 16-step reference; the
  default cfg for FLUX.3 was fixed to 1.0 after the default over-saturated (`wave-f/`).
- **Control plane**: model registry, lifecycle state machine, job queue, cancellation, memory-budget ledger,
  approved-root path guard.
- **HTTP API** (loopback + bearer token by default, size limits), **MCP stdio** (models, chat, vision, image, runtime,
  jobs, benchmark) and the **embedded web UI "HCS Studio"** (HTTP + headless-browser boot proven).
- **Benchmarks**: `benchmark --suite quick|full`, `POST /hcs/v1/benchmarks`, MCP `hcs.runtime.benchmark`, B0/B1
  baselines (CPU vs Vulkan) with the AGENTS.md 21.1 JSON metadata.
- **Hardening**: audit fixes D1-D7, soak defects D-S1..D-S6 (red -> green), simulated OOM / USB-unplug / shutdown tests
  (wave G), cpp-httplib bumped to v0.44.0 after an OSV query, supply-chain scans of npm/cargo/native pins.
- **Packaging**: portable folder + ZIP + `SHA256SUMS.txt` + CycloneDX SBOM built by `scripts/package.ps1`; CI builds and
  verifies it on a hosted runner.
- **Experimental HCS Vulkan kernel** (Q8_0 GEMV): CPU reference, SPIR-V validated, validation layers clean, device-lost
  fallback; **OFF by default** because it is 18.2x slower than the upstream GGML Vulkan kernel on 4096x14336
  (`docs/vulkan-kernels.md`).

## What is NOT in this release / BLOCKED / NOT RUN

| Item | Status | Why |
|---|---|---|
| Clean-machine test of the single-file EXE | NOT RUN (restricted-environment run PASS for both EXEs) | Windows Sandbox is not installed and enabling it needs elevation; no clean VM exists on this machine |
| Code signing of `HCSInfer.exe` / wheel | NOT RUN | no certificate; binaries are unsigned |
| PyPI upload, GitHub release, Vercel deploy | NOT RUN | owner-side; nothing was pushed, tagged, uploaded or deployed |
| `release.yml` on a hosted runner (incl. Vulkan SDK install step) | NOT RUN | workflow validated with actionlint only; never executed |
| MSIX / installer | skipped | no free official packaging tool present on the build machine |
| Model Vault in the engine load path (M5 Stage B), B2/B5/B6/B8 | BLOCKED | needs a model larger than RAM and an administrator page-cache flush |
| Physical USB cable pull; real RAM exhaustion | NOT RUN | only simulated (injected) tests exist |
| Full soak profile (100 cycles, 30 min, long ASan fuzz), Vulkan soak | NOT RUN | only the quick profile was run |
| Code signing | NOT RUN | no certificate |
| `hardware.yml` self-hosted Radeon CI | NOT RUN | never executed |
| Real `prime-agent` session, real LoRA training run | NOT RUN | external sidecar / trainer not run (ADR-011, ADR-012) |
| Real WebGPU inference, Vercel deploy of `web/` | NOT RUN | headless CI has no WebGPU adapter; deploy needs owner credentials |
| Interactive GUI (playwright) phase | NOT RUN | playwright not installed on the machine |
| MCP Streamable HTTP and its Origin validation | NOT RUN | intentionally not implemented (stdio only) |
| `models load`, `models unload`, `tune` CLI commands | Planned | exit code 10 (load/unload works over HTTP and MCP) |
| Scheduler, telemetry module, M9 research features | Planned / NOT RUN | no measured benefit to justify them yet |
| LPIPS for the FLUX.3 step sweep, `params-backend disk` for FLUX.3, repeats | NOT RUN | single samples only |
| Literal `cargo deny`, scans of vendored stb/libwebp/oniguruma/utf8proc | NOT RUN | tool not installed / no scanner |
| Linux / Mesa RADV, other GPUs, ROCm | NOT RUN | one device only |
| FLUX.3 with the official BFL text encoder/VAE | BLOCKED | gated files not accessible to the configured account |

## Licences and models

Weights are never bundled. FLUX.3 Klein uses the FLUX Kommunity License (non-commercial unless a qualifying user);
LFM3-Omni uses the LFM Open License v1.0. See `THIRD_PARTY_NOTICES.md`, `docs/licence-audit.md` and
`docs/supported-models.md`.

## Upgrade / usage

Build with `docs/build.md`; unzip the portable package, put models in an approved root, run `HCSInfer doctor`.
The API binds to 127.0.0.1 and requires the generated bearer token.
