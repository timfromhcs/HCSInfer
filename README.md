# HCSInfer

HCSInfer is a local-first runtime for text chat, image understanding and image generation on Windows 11
(target hardware: Ryzen 7 7735HS + Radeon 680M, Vulkan, external USB drive as model vault). It reuses
`llama.cpp`/GGML and `stable-diffusion.cpp` behind one control plane (model registry, job queue,
memory budget, HTTP API, MCP).

**Status: alpha (pre-V1).** Real text generation (CPU and Vulkan), real image generation/editing (CLI, CPU and
Vulkan), a loopback HTTP API and an MCP stdio server run on the target machine. **GUI, HTTP/MCP image
generation routes, benchmarks and custom Vulkan kernels are not implemented.** V1 is not released. See
`docs/roadmap/STATUS.md` for the evidence behind every row below.

## Feature matrix

| Feature | State | Notes |
|---|---|---|
| Hardware report `HCSInfer doctor [--json]` (CPU features, RAM, OS, GPU adapters, Vulkan devices/heaps, drive info) | Implemented | evidence `docs/test-evidence/M0`; unknown values print `unknown` |
| Text chat (`chat`, llama.cpp adapter), CPU | Implemented | real GGUF tested (Qwen3 0.6B Q4_K_M) |
| Text chat, Vulkan (Radeon 680M) | Implemented | requires the `windows-msvc-release-vulkan` build; explicit `--backend vulkan` fails loudly instead of falling back |
| Image generation + img2img (`image generate/edit`, stable-diffusion.cpp adapter), CPU and Vulkan | Implemented (CLI) | tested with SD1.5 Q8_0 only; other model families not tested |
| Vision / image understanding (libmtmd): `HCSInfer vision`, CPU and Vulkan | Implemented (alpha) | tested with SmolVLM2-500M Q8_0 + mmproj only; one image per request, no video/audio; `docs/test-evidence/M2-vision/` |
| Model registry, lifecycle, job queue, cancellation, memory-budget arithmetic, approved-root path guard | Implemented | |
| HTTP API: `/v1/models`, `/v1/chat/completions` (streaming), `/hcs/v1/*` | Experimental | loopback by default, bearer token, size limits; `docs/api.md` |
| HTTP API: image input on chat (`image_url` data URIs, streaming, cancellation) | Experimental | needs a model + its mmproj in one folder; `docs/api.md` |
| HTTP API: `/v1/images/*`, `POST /hcs/v1/benchmarks` | Planned | return `501` today |
| MCP stdio: models, chat, runtime, jobs tools | Experimental | `docs/mcp.md` |
| MCP: `hcs.vision.analyze` | Experimental | real VLM, path guarded; `docs/mcp.md` |
| MCP: `hcs.image.generate`, `hcs.image.edit` | Planned | listed, return `unsupported` today |
| MCP Streamable HTTP | Planned | intentionally not implemented |
| GUI | Planned | design proposal in ADR-009 (local web UI served by the API); nothing built |
| Model Vault: bounded async sequential reads, SHA-256 (`src/storage`) | Experimental | tested read-only on a real GGUF; the engines do not load through it yet |
| HCSMODEL chunk container (Raw/LZ4/Zstd, per-chunk hashes) | Experimental | a measured loading benefit on the HDD is not proven |
| Shared GGML (one GGML for both engines, `SD_USE_UPSTREAM_GGML=ON`) | Experimental | SD1.5 output matched the patched build once; other models untested |
| Portable folder package + SHA-256 + SBOM (`scripts/package.ps1`, `scripts/sbom.ps1`) | Experimental | not a single-file EXE |
| Single `HCSInfer.exe` | Planned, unproven | dynamic CRT today, no clean-machine test (ADR-008) |
| `benchmark`, `tune`, `cache` CLI commands | Planned | exit code 10 |
| Custom Vulkan kernels, scheduler, advanced research features | Planned | only with measured benefit |
| CI (GitHub Actions) | Created, never run | `.github/workflows/`; the repository has no commits or remote yet |

Legend: Implemented / Experimental / Planned / Unsupported.

## Requirements

- Windows 11 x64, Visual Studio 2022 (Desktop development with C++, MSVC v143), CMake >= 3.25, Ninja, Git.
- Vulkan build: LunarG Vulkan SDK (build time only; version used so far: 1.4.357.0). At run time the installed
  AMD/OEM graphics driver provides Vulkan; HCSInfer never bundles or replaces it.
- Verified only on: Ryzen 7 7735HS, Radeon 680M (AMD proprietary driver), Windows 11 build 26200. Other GPUs are untested.
- Radeon 680M uses shared system memory: CPU and GPU compete for RAM capacity and bandwidth. The Vulkan heap
  size reported by the driver is not extra RAM.

## Quick start

```powershell
git clone --recurse-submodules <repo> ; cd HCSInfer
.\scripts\build.ps1 -Preset windows-msvc-release -RunTests            # CPU build
.\scripts\build.ps1 -Preset windows-msvc-release-vulkan              # Vulkan build (needs the Vulkan SDK)
$exe = '.\build\windows-msvc-release\bin\HCSInfer.exe'
& $exe --help
& $exe doctor --json
& $exe chat --model E:\models\model.gguf --prompt "Hello" --backend cpu
& $exe image generate --model D:\models\sd15-q8_0.gguf --prompt "a red apple on a plain table" `
   --width 256 --height 256 --steps 2 --backend cpu --json
& $exe serve --model-root E:\models                                   # 127.0.0.1:8765, token printed on stderr
& $exe mcp stdio --model-root E:\models
```

Exact build commands and troubleshooting: `docs/build.md`. Tests that need a real model skip (and say so) unless
`HCS_TEST_GGUF` points to a GGUF file.

## Models and licences

Models are never part of this repository or the executable. You are responsible for the licence of every model you
download. `models scan` only reads headers; it never writes into your model folder.

## Model Vault and storage

The model folder may live on an external USB drive. HCSInfer does not create a decompressed model cache on the SSD.
Windows may still page memory to the system page file; HCSInfer does not change that. A USB HDD is a cold archive,
not RAM: a cold first load of a multi-GB model is slow (a cold SD1.5 first step took about 218 s in our run; warm
runs took seconds), and cold/warm numbers must never be compared. See `docs/model-vault.md`.

## Security and privacy (current behaviour)

- The API binds to `127.0.0.1` by default, requires a bearer token, validates `Host`/`Origin`, and caps body size and
  concurrency. LAN exposure is an explicit opt-in (`--lan`).
- MCP and API file arguments go through the approved-root guard (`..`, symlink/junction escape rejected). No shell or
  arbitrary file tool exists.
- Logs go to stderr as JSON lines and never contain prompts or images.
- No network access at run time. The build fetches pinned GoogleTest, nlohmann/json, cpp-httplib, LZ4 and Zstandard.

See `SECURITY.md`.

## Benchmarks

Only provisional numbers exist (uncontrolled, few samples): `docs/test-evidence/M1/llama/README.md`,
`docs/test-evidence/M6-storage/`. Methodology for future results: `docs/decisions/ADR-010-benchmark-methodology.md`.
No performance claim is made beyond those files.

## Known limitations

- Vision is limited to one image per single-turn request and one tested model family (SmolVLM2); no GUI, no benchmark command, no custom kernels.
- Image models other than SD1.5 Q8_0 are untested; FP8/INT8 diffusion paths are unavailable with upstream GGML.
- Output of Vulkan diffusion was once not byte-reproducible run to run (not investigated).
- Real-model tests skip without `HCS_TEST_GGUF`; GPU tests only run on the Vulkan build.
- `models scan` rejects relative paths containing `..` by design.

## Troubleshooting

- *Vulkan missing or `--backend vulkan` fails:* run `HCSInfer doctor`; install/repair the AMD driver; use `--backend cpu`.
- *Out of memory on load:* unload the previous model, use a smaller quantization, lower context/resolution.
- *Model unsupported:* the adapter returns a clear `Unsupported` error (exit code 10); this is not a crash.
- *USB drive disconnected during load:* load fails; handling under unplug is not yet tested.

## Documentation

`docs/architecture.md`, `docs/build.md`, `docs/api.md`, `docs/mcp.md`, `docs/model-vault.md`,
`docs/hardware-support.md`, `docs/roadmap/IMPLEMENTATION_PLAN.md`, `docs/roadmap/STATUS.md`, `docs/decisions/` (ADRs),
`AGENTS.md` (binding engineering specification).

## Licence

MIT, see `LICENSE`. Third-party notices: `THIRD_PARTY_NOTICES.md`.
