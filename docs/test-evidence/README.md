# Test evidence index

Evidence is grouped per milestone. Each directory has a `README.md` explaining what was run, on which machine and
build, what passed, and what is NOT RUN / BLOCKED. Raw logs, JSON results and (where useful) produced images are kept
next to it.

| Directory | Milestone / scope |
|---|---|
| `M0/` | hardware + repository baseline (`doctor.json`, `doctor-no-vulkan.json`, USB-HDD read benchmark, `vulkaninfo`) |
| `M1/` | independent upstream engine proof: `llama/` (Vulkan build + real GGUF), `sd/` (stable-diffusion.cpp, patched vs upstream GGML) |
| `M2-core/` | headless core: lifecycle, registry, JobQueue, MemoryBudget, PathGuard, settings |
| `M2/` | control-plane facade, llama adapter, diffusion adapter, M7 shared-GGML proof |
| `M2-vision/` | libmtmd vision adapter (SmolVLM2-500M Q8_0 + mmproj), CPU + Vulkan, and the HTTP/MCP vision end-to-end |
| `M3-api-mcp/` | HTTP API + MCP stdio end-to-end with a real text model |
| `mcp-benchmark/` | MCP `hcs.runtime.benchmark` tool: real GGUF over stdio, AGENTS.md 21.1 document returned |
| `M3-image/` | HTTP image routes + MCP image tools with a real diffusion model (47/47 checks) |
| `M4-gui/` | embedded web UI: unit + contract + socket + headless-browser checks |
| `M6-storage/` | HCSMODEL container, chunk codecs, parser mutation tests, storage benchmark |
| `M10/` | security audit report |
| `M10-fixes/` | the audit defects D1-D7, red/green test pairs and the full-suite logs |
| `benchmark/` | real `HCSInfer benchmark --suite quick` runs (B0 CPU + B1 Vulkan, AGENTS.md 21.1 JSON) |
| `M5-loadmodes/` | llama.cpp weight-load-mode matrix (auto/mmap/none/mlock/mmap+mlock/direct_io) with peak RAM/commit |
| `B7-params/` | stable-diffusion.cpp compute backend vs parameter backend (Vulkan/CPU x cpu/disk params), CLI stage timings |
| `static-analysis/` | first clang-tidy run, its triage, the clean default-gate run and the CI job's result |
| `package/` | first real `scripts/package.ps1` run: portable folder + ZIP, SBOM, SHA-256, licence texts, standalone run |
| `ci/` | what the CI gates check and which tool versions they were verified with (clang-format pinning) |
| `lfm3/` | LFM3-Omni-2B Q8_0: chat, streaming, cancellation, vision, OpenAI tool calling, 40-task agentic eval, quick benchmarks (CPU + Vulkan) |
| `flux3/` | FLUX.3 Image Klein plumbing and SD1.5 regression; the render itself is BLOCKED (gated encoder/VAE) |
| `studio/` | HCS Studio UI screenshots (desktop and phone, light and dark), real model loads through the UI |
| `agent/` | HCS Agent policy/provider/launch-plan/process-supervisor build and test logs (a real agent session is NOT RUN) |
| `train/` | HCS Train CLI demo and test notes (a real training run is NOT RUN) |
| `web/` | `web/` headless Chrome boot check (`web-boot.json/.txt`, `web-chat.png`); real WebGPU inference is NOT RUN |
| `W4-static-exe/` | static-CRT `/MT` + `GGML_OPENMP=OFF` single-EXE experiment: build, 783/783 tests, `dumpbin` (system DLLs only), real llama/SD1.5 runs; clean-machine test NOT RUN |
| `vulkan-kernels/` | M8 Q8_0 GEMV: SPIR-V validation, `kernel-cpu`/`kernel-vulkan` correctness (4/4 on the real 680M), isolated CPU-vs-Vulkan benchmark (feature-flagged, not default-enabled) |

## Redaction policy

These files are published with the repository, so:

* **Ephemeral local API tokens are redacted.** The end-to-end scripts capture the `serve` stderr, which contains the
  one-per-run bearer token of a loopback server (`HCSInfer API token: ...`). Those tokens are dead by the time the
  evidence is committed, but they are credential-shaped, so the value is replaced with `<redacted-run-token>`.
  Only the value is replaced; the check names, exit codes and results are untouched.
* **Model SHA-256 hashes are kept on purpose** - they identify the exact model file a result was measured with.
* Device, driver and GPU identifiers are kept: they are the point of a hardware-aware project.

Evidence files still contain absolute paths of the machine that produced them (model locations such as
`E:\AILOCALTRAIN\...`, `<MODELS>\...`), because the exact file is part of reproducing a result. They contain no
credentials, no user documents and no model data.

Per AGENTS.md 16.2: mocks are never accepted as evidence for inference, model loading, Vulkan execution, image
generation or MCP end-to-end behaviour. Every directory above that claims one of those states which real model and
machine was used, and marks what is NOT RUN.
- `wave-g/` - hardening gaps: OOM injection, USB-disconnect simulation, shutdown (fake + real model), supply-chain scans (npm, cargo audit, OSV, licences), restricted-environment static-EXE run (2026-11-20).
- `wave-a/` - real-model verification and GUI browser interaction (playwright) on the CPU and Vulkan builds (2026-11-20).
- `wave-b/` - single-file static EXEs, Python wheel/sdist, portable ZIP, release workflow lint (2026-11-20).
- `wave-c/` - HCS Studio polish: Markdown safety, real-browser screenshots (2026-11-20).
- `benchmarks/` - wave D: benchmark JSON of the final static EXEs (text, KV cache, vision, image, API, drive reads); report in `docs/benchmarks.md`.
- `wave-r/` - release-candidate 0.1.0 verification (superseded by `release/`).
- `release/` - **1.0.0-rc.1 final verification**: format, clang-tidy, full CTest on the CPU and Vulkan builds with real models, web and Python tests, packaging, wheel, SHA-256 hashes; see `release/RESULTS.md`.
