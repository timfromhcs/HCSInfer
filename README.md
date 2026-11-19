# HCSInfer

HCSInfer is a local-first runtime for text chat, image understanding and image generation on Windows 11
(target hardware: Ryzen 7 7735HS + Radeon 680M, Vulkan, external USB drive as model vault). It reuses
`llama.cpp`/GGML and `stable-diffusion.cpp` behind one control plane (model registry, job queue,
memory budget, HTTP API, MCP).

**Status: alpha (pre-V1, V1.1 work in progress).** Real text generation (CPU and Vulkan) with OpenAI-style tool calls,
real image generation/editing (CLI, HTTP, MCP, GUI; CPU and Vulkan, proven with SD1.5), image understanding, a
loopback HTTP API, an MCP stdio server, an embedded local web UI (HCS Studio), a browser-only WebGPU/WASM demo
(`web/`, ADR-013), a local fine-tuning control plane (`HCSInfer train`, ADR-012), the HCS Agent policy and process
supervisor (ADR-011) and a real text benchmark command run on the target machine.
**Custom Vulkan kernels, the Model Vault in the engine load path, the single-file EXE, `tune`,
`models load|unload` as CLI commands and Streamable HTTP are not
implemented. The FLUX.3 Klein render, a real `prime-agent` session, a real training run and real WebGPU inference
are NOT RUN / BLOCKED.** V1 is not released. See `docs/roadmap/STATUS.md` (it also holds the V1 release checklist)
for the evidence behind every row below.

## Feature matrix

| Feature | State | Notes |
|---|---|---|
| Hardware report `HCSInfer doctor [--json]` (CPU features, RAM, OS, GPU adapters, Vulkan devices/heaps, drive info) | Implemented | evidence `docs/test-evidence/M0`; unknown values print `unknown` |
| Text chat (`chat`, llama.cpp adapter), CPU | Implemented | real GGUF tested (Qwen3 0.6B Q4_K_M, LFM3-Omni-2B Q8_0) |
| Text chat, Vulkan (Radeon 680M) | Implemented | requires the `windows-msvc-release-vulkan` build; explicit `--backend vulkan` fails loudly instead of falling back |
| Image generation + img2img (`image generate/edit`, stable-diffusion.cpp adapter), CPU and Vulkan | Implemented (CLI) | tested with SD1.5 Q8_0 only; other model families not tested |
| Vision / image understanding (libmtmd): `HCSInfer vision`, CPU and Vulkan | Implemented (alpha) | tested with SmolVLM2-500M Q8_0 and LFM3-Omni-2B (+ their mmproj); one image per request, no video/audio; `docs/test-evidence/M2-vision/`, `lfm3/` |
| FLUX.3 Image Klein through the diffusion adapter (`--llm`, `--vae`, `--preset fast\|quality`, `.components.json` sidecar) | Experimental plumbing; **render BLOCKED** | the pinned stable-diffusion.cpp recognises the model, but the gated encoder/VAE could not be downloaded, so no FLUX.3 image has been produced; the `fast` preset is provisional and unmeasured; `docs/supported-models.md`, `docs/test-evidence/flux3/` |
| Model registry, lifecycle, job queue, cancellation, memory-budget arithmetic, approved-root path guard | Implemented | |
| HTTP API: `/v1/models`, `/v1/chat/completions` (streaming), `/hcs/v1/*` | Experimental | loopback by default, bearer token, size limits; `docs/api.md` |
| HTTP API: image input on chat (`image_url` data URIs, streaming, cancellation) | Experimental | needs a model + its mmproj in one folder; `docs/api.md` |
| HTTP API: `/v1/images/generations`, `/v1/images/edits` | Experimental | real stable-diffusion.cpp render on CPU **and Vulkan** (47/47 checks on both builds); `b64_json` only, no `url`/`mask` (`501`); JSON body only (`multipart` -> `415`); `docs/api.md` |
| HTTP API: OpenAI tool calling (`tools`, `tool_calls`, `tool_choice` auto/none; non-stream and SSE) | Experimental | real LFM3-Omni run on CPU and Vulkan, 37/40 on the project's own 40-task eval; `tool_choice` required / named function and legacy `functions` answer `501`; `docs/api.md`, `docs/test-evidence/lfm3/` |
| HTTP API: `POST /hcs/v1/benchmarks` | Implemented | asynchronous job (`202` + `status_url`), cancellable; contract-tested, real-model run NOT RUN |
| MCP stdio: models, chat, runtime, jobs, benchmark tools | Experimental | `docs/mcp.md` |
| MCP: `hcs.runtime.benchmark` | Experimental | shared benchmark runner as a job; real LFM3-Omni-2B run recorded |
| MCP: `hcs.vision.analyze` | Experimental | real VLM, path guarded; `docs/mcp.md` |
| MCP: `hcs.image.generate`, `hcs.image.edit` | Experimental | real render, image returned as an MCP image content block; `docs/mcp.md` |
| MCP Streamable HTTP | Planned | intentionally not implemented |
| HCS Studio GUI: embedded local web UI (`GET /`, `/ui/*` on the same loopback listener) | Experimental | Chat (with tool-call display), Vision, Image Studio (text-to-image and image-to-image), Models, Performance, Settings, Simple+Advanced; the Agent and Train tabs are labelled Planned and contain no controls; no framework, no CDN; `docs/gui.md`, ADR-009 |
| HCS Studio Web (`web/`): browser-only WebGPU/WASM demo, no server | Experimental | static site, no server inference and no analytics; Chat/Vision/Models/Performance/Settings run in the tab, Image Studio/Train/Agent are "Local only"; the shell + capability probe are proven in a real headless Chrome, **real WebGPU inference is NOT RUN here** (headless has no adapter); `web/README.md`, ADR-013 |
| Model Vault: bounded async sequential reads, SHA-256 (`src/storage`) | Experimental | tested read-only on a real GGUF; the engines do not load through it yet |
| HCSMODEL chunk container (Raw/LZ4/Zstd, per-chunk hashes) | Experimental | a measured loading benefit on the HDD is not proven |
| Shared GGML (one GGML for both engines, `SD_USE_UPSTREAM_GGML=ON`) | Experimental | SD1.5 output matched the patched build once; other models untested |
| Portable folder package + SHA-256 + SBOM (`scripts/package.ps1`, `scripts/sbom.ps1`) | Experimental | not a single-file EXE |
| Single `HCSInfer.exe` | Planned, unproven | dynamic CRT today, no clean-machine test (ADR-008) |
| `benchmark` CLI (`--suite quick`): real text benchmark, AGENTS.md 21.1 JSON | Implemented | cold+warm phases, median/p90, real metrics from the engine |
| `benchmark --suite full` (prefill/decode sweeps, `--sweep-sizes`) | Implemented | `SKIPPED`/`FAIL` points, `result_status` `PARTIAL`; contract-tested, real-model run NOT RUN |
| `cache stats` / `cache clear [--yes] [--json]` | Implemented | only HCS-owned state-dir `cache/`, `scratch/` and orphaned `.tmp.<n>` files; exit 2 without `--yes`, 8 if partial |
| HCS Train: `train validate\|gate\|adapter\|pipeline` (dataset validation, eval gate, adapter bookkeeping) | Implemented (logic) | a trainer is external (ADR-012) and has not been run; promotion is refused unless the gate kept the adapter; `docs/train.md` |
| HCS Agent foundations: tool-call policy, loopback provider document, launch plan, process supervisor (Job Object tree-kill) | Implemented (logic + supervision) | a real `prime-agent` sidecar session is NOT RUN; `docs/agent.md`, ADR-011 |
| `tune`, `models load`, `models unload` CLI commands | Planned | exit code 10 (model load/unload works over HTTP and MCP) |
| Custom Vulkan kernels, scheduler, advanced research features | Planned | only with measured benefit |
| CI (GitHub Actions) | Green | `.github/workflows/`; public repo `timfromhcs/HCSInfer`. `pr-quick-gate` run 44167798984 green on `main` (clang-format, clang-tidy, SBOM, `web`, Release and Debug build+ctest); model/GPU tests skip there and stay NOT RUN. The self-hosted `hardware.yml` has never run |

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
git clone --recurse-submodules https://github.com/timfromhcs/HCSInfer.git ; cd HCSInfer
.\scripts\build.ps1 -Preset windows-msvc-release -RunTests            # CPU build
.\scripts\build.ps1 -Preset windows-msvc-release-vulkan              # Vulkan build (needs the Vulkan SDK)
$exe = '.\build\windows-msvc-release\bin\HCSInfer.exe'
& $exe --help
& $exe doctor --json
& $exe chat --model E:\models\model.gguf --prompt "Hello" --backend cpu
& $exe image generate --model D:\models\sd15-q8_0.gguf --prompt "a red apple on a plain table" `
   --width 256 --height 256 --steps 2 --backend cpu --json
& $exe serve --model-root E:\models                                   # 127.0.0.1:8765, token printed on stderr
                                                                    # open the printed web UI URL, or use the API
& $exe benchmark --suite quick --model E:\models\model.gguf --backend cpu --export bench.json
& $exe benchmark --suite full --model E:\models\model.gguf --backend cpu --sweep-sizes 128,512
& $exe cache stats --json          # what HCS itself keeps as cache; `cache clear --yes` deletes exactly that
& $exe benchmark --suite quick --model E:\models\model.gguf --backend cpu --load-mode auto   # or none|mmap|mlock|mmap+mlock|direct_io
& $exe mcp stdio --model-root E:\models
```

The native CTest suite registered 518 tests at the last recorded run; the 46 `web/` tests run separately with `npm test`.
Exact build commands and troubleshooting: `docs/build.md`. Tests that need a real model skip (and say so) unless the
matching variable points at one: `HCS_TEST_GGUF` (text), `HCS_TEST_VLM` + `HCS_TEST_MMPROJ` (vision),
`HCS_TEST_DIFFUSION_MODEL` (image).

## Models and licences

Models are never part of this repository or the executable. You are responsible for the licence of every model you
download. `models scan` only reads headers; it never writes into your model folder.

### Importing models

1. Put the files in a folder you control (it may be on an external drive). A vision model needs its `mmproj*.gguf`
   projector **in the same folder**; FLUX.3 Klein needs its text encoder and VAE (`--llm`, `--vae` or a
   `<model>.components.json` sidecar, see `docs/supported-models.md`).
2. Register them: `HCSInfer models scan E:\models` (read-only: validates GGUF/safetensors headers and writes only into
   the state directory), then check with `HCSInfer models list`.
3. Use the model id (not a path) from the API, MCP and GUI; `chat`, `vision` and `image` also accept a file path.
   The server and MCP only load files that resolve inside an approved root (`--model-root`, or `model_roots` in
   `settings.json`).

Check the licence of every model before use. Two licences in the current evidence are restrictive: LFM3-Omni uses the
LFM Open License v1.0 (commercial use limited by a revenue threshold) and FLUX.3 Klein uses the FLUX Kommunity
licence (`THIRD_PARTY_NOTICES.md`).

## API and MCP configuration

```powershell
# HTTP API: loopback, random bearer token printed on stderr (or pass --token / set HCS_API_TOKEN)
& $exe serve --model-root E:\models
curl.exe http://127.0.0.1:8765/hcs/v1/health                                  # no auth, {"status","version"}
curl.exe -H "Authorization: Bearer <token>" http://127.0.0.1:8765/v1/models
curl.exe -H "Authorization: Bearer <token>" -H "Content-Type: application/json" `
  -d '{"model":"<id>","messages":[{"role":"user","content":"Hello"}],"max_tokens":32}' `
  http://127.0.0.1:8765/v1/chat/completions
```

MCP is stdio only. A generic client entry (client config formats differ; only a scripted stdio client has been run
against it, no third-party MCP client has):

```json
{ "command": "C:\\path\\to\\HCSInfer.exe", "args": ["mcp", "stdio", "--model-root", "E:\\models"] }
```

Routes, options and error codes: `docs/api.md`; tools and protocol versions: `docs/mcp.md`.

## Model Vault and storage

Scope: the Model Vault (`src/storage`: read-only validation, bounded asynchronous sequential reads, SHA-256, the
optional HCSMODEL container) is experimental and **not yet in the engine load path**; the engines load GGUF files
themselves (llama.cpp `mmap` by default, other load modes through `--load-mode`). The model folder may live on an
external USB drive. HCSInfer does not create a decompressed model cache on the SSD.
Windows may still page memory to the system page file; HCSInfer does not change that. A USB HDD is a cold archive,
not RAM: a cold first load of a multi-GB model is slow (a cold SD1.5 first step took about 218 s in our run; warm
runs took seconds), and cold/warm numbers must never be compared. See `docs/model-vault.md`.

## HCS Studio Web (browser demo)

`web/` is a separate deliverable (not part of the native EXE): a static site whose inference runs entirely in the
browser through WebGPU, with a WebAssembly fallback. No prompt, image or model file leaves the tab; there is no
server inference and no analytics. Chat, Vision, Models, Performance and Settings run in the browser; Image Studio,
Train, the shell-based Agent and the Model Vault are shown as "Local only". The one third-party runtime
(`@huggingface/transformers` 4.3.2, Apache-2.0) is version-pinned and loaded from a CDN.

```powershell
cd web
npm test                            # 46 unit + static security tests (no dependencies)
node tests/e2e/boot.mjs             # real static server + real headless Chrome (14 checks)
node tools/serve.mjs 8080           # then open http://127.0.0.1:8080/
```

The shell and the capability probe are verified in a real headless Chrome; **real WebGPU inference is NOT RUN in
this environment** (headless has no WebGPU adapter) and must be tested in a real WebGPU browser. Details, security
model and the Vercel deploy (`NOT RUN`, needs owner credentials): `web/README.md`, ADR-013.

## Security and privacy (current behaviour)

- The API binds to `127.0.0.1` by default, requires a bearer token, validates `Host`/`Origin`, and caps body size and
  concurrency. LAN exposure is an explicit opt-in (`--lan`).
- MCP and API file arguments go through the approved-root guard (`..`, symlink/junction escape, reserved Windows
  device names such as `NUL`/`CON`, trailing dots/spaces rejected). No shell or arbitrary file tool exists.
- Untrusted JSON bodies and MCP lines are rejected before parsing when they nest too deeply (stack-exhaustion guard);
  image dimensions/size and the number of concurrent jobs are bounded.
- Logs go to stderr as JSON lines and never contain prompts or images.
- No network access at run time. The build fetches pinned GoogleTest, nlohmann/json, cpp-httplib, LZ4, Zstandard and Vulkan-Headers.
- The agent sidecar is designed to be loopback-only and offline, started without a shell inside a kill-on-close Job
  Object (`docs/agent.md`); a real `prime-agent` session has not been run.
- Privacy: prompts, completions and images are not logged and there is no telemetry; `web/` contacts only
  `huggingface.co` (model files) and one pinned CDN script. The API token is per run and never embedded in the UI.
- Not done yet (NOT RUN): a dependency vulnerability scan, long/scheduled fuzzing and a formal licence review.

See `SECURITY.md`.

## Benchmarks

`HCSInfer benchmark --suite quick --model <gguf>` measures real load time, TTFT, prompt and decode tokens/s
(cold process phase + warm phase with warm-up, median/p90 and the raw samples) and prints one AGENTS.md 21.1 JSON
document with machine, model hash, quantization, backend and storage metadata. Unmeasurable values are the string
`unknown`, never an estimate. `--suite full` is not implemented yet (it exits with an error).

Two real baselines are recorded on the target machine with the same model (`qwen3-0.6b` Q4_K_M), prompt, seed and
commit: **B0 CPU** (warm decode 66.1 t/s, warm TTFT 369 ms) and **B1 Vulkan/Radeon 680M** (warm decode 126.1 t/s,
warm TTFT 134 ms). Both are 3-iteration medians with an uncontrolled OS file cache and an uncontrolled Vulkan
shader cache - see `docs/test-evidence/benchmark/README.md` for the full tables and the caveats.

Method, the load-mode matrix and the diffusion parameter-backend run are summarised in `docs/benchmarks.md`.
Earlier provisional numbers (uncontrolled, few samples): `docs/test-evidence/M1/llama/README.md`,
`docs/test-evidence/M6-storage/`. Methodology: `docs/decisions/ADR-010-benchmark-methodology.md`.
No performance claim is made beyond those files.

## Known limitations

- Vision is limited to one image per single-turn request and one tested model family (SmolVLM2); no custom kernels and no scheduler beyond the single-worker job queue.
- Image routes are text-to-image and img2img only (`b64_json`); `url` responses, `mask`/inpainting and `multipart/form-data` uploads are rejected with a clear error.
- The GUI is served on the loopback API listener and needs a browser; model load and rendering show an indeterminate busy state, not real progress. Its playwright interaction phase and a Vulkan-build UI run are NOT RUN.
- No image model other than SD1.5 Q8_0 has produced output; FLUX.3 Klein is BLOCKED (gated encoder/VAE). FP8/INT8 diffusion paths are unavailable with upstream GGML.
- Tool calling: the tested model emits one call per turn (parallel calls 0/3 on the eval); `tool_choice` required / named function is `501`.
- LFM3-Omni audio/video input and speech output are not wired. No real training run, no real `prime-agent` session and no real WebGPU inference have been executed.
- One model resident and one job at a time (single worker). USB-unplug and OOM-injection behaviour are not tested.
- Output of Vulkan diffusion was once not byte-reproducible run to run (not investigated).
- Real-model tests skip without `HCS_TEST_GGUF`; GPU tests only run on the Vulkan build.
- `models scan` rejects relative paths containing `..` by design.

## Troubleshooting

- *Vulkan missing or `--backend vulkan` fails:* run `HCSInfer doctor`; install/repair the AMD driver; use `--backend cpu`.
- *Out of memory on load:* unload the previous model, use a smaller quantization, lower context/resolution.
- *Model unsupported:* the adapter returns a clear `Unsupported` error (exit code 10); this is not a crash.
- *USB drive disconnected during load:* load fails; handling under unplug is not yet tested.
- *`401` or `403` from the API:* use the token printed on stderr by `serve` (`Authorization: Bearer ...`); the `Host` header must be loopback and a browser `Origin` must be this server's.
- *`501 unsupported` for a vision request:* the model has no `mmproj*.gguf` in its folder. *`501` on the image routes:* the build has no image engine.
- *FLUX.3 load fails and asks for `--llm`/`--vae`:* it is a multi-file model; see `docs/supported-models.md`.
- *The CLI says "planned" (exit code 10):* `tune`, `models load|unload` are not implemented; load models through `serve`/`mcp`, or give `chat`/`vision`/`image` a model path.
- *Very slow first load from the USB drive:* the tested drive reads about 65 MiB/s raw; run again for a warm figure and never compare cold with warm.

## Documentation

`docs/architecture.md`, `docs/build.md`, `docs/api.md`, `docs/mcp.md`, `docs/model-vault.md`,
`docs/hardware-support.md`, `docs/gui.md`, `docs/supported-models.md`, `docs/benchmarks.md`, `docs/train.md`,
`docs/licence-audit.md`,
`docs/agent.md`, `docs/static-analysis.md`, `web/README.md`, `CHANGELOG.md`, `SECURITY.md`, `CONTRIBUTING.md`,
`docs/roadmap/IMPLEMENTATION_PLAN.md`, `docs/roadmap/STATUS.md` (with the V1 release checklist),
`docs/decisions/README.md` (ADR index), `docs/test-evidence/README.md`, `AGENTS.md` (binding engineering specification).

## Licence

MIT, see `LICENSE`. Third-party notices: `THIRD_PARTY_NOTICES.md`.
