# HCSInfer — Autonomous Engineering Agent Instructions

> **Mission:** Build HCSInfer from the current repository state into a stable, secure, reproducible, hardware-aware local AI runtime for text, vision and image generation. Work autonomously from repository inspection through implementation, testing, hardening, documentation and a release candidate. Do not stop after writing a plan or after the first successful build. Continue in bounded self-healing loops until every feasible V1 gate is either proven green or explicitly recorded as blocked by a real external dependency, unavailable hardware/model, licence constraint or missing user decision.
>
> **Target-first hardware:** Windows 11 x64, AMD Ryzen 7 7735HS (8C/16T, Zen 3+, AVX2/FMA3) and Radeon 680M (RDNA 2, 12 CUs / 768 shaders), shared system memory, Vulkan, and an external USB 3.x HDD used as the model vault. Linux/Mesa RADV is a later supported target. Detect the actual machine at runtime; never hard-code the target specs as detected facts.
>
> **Product principle:** One application and one control plane for chat, vision and image generation. Reuse `llama.cpp`/GGML and `stable-diffusion.cpp` until a measured reason exists to replace or extend an operator. Build original HCSInfer memory, storage, scheduling, API/MCP, UI and selective Vulkan-kernel technology on top of proven engines.

---

## 0. Instructions to the autonomous coding agent

### 0.1 You own the complete engineering loop

You are the implementation agent, test engineer, build engineer, documentation maintainer and release-hardening agent. Once started, independently:

1. Inspect the repository, branches, working tree, project files, current implementation and all existing tests.
2. Preserve user changes. Never reset, clean, overwrite or force-push uncommitted work. If the working tree is dirty, inspect and preserve it; isolate your work on a safe branch or separate worktree when appropriate.
3. Convert this specification into a concrete issue/task checklist and a dependency graph. Save it to `docs/roadmap/IMPLEMENTATION_PLAN.md` and maintain `docs/roadmap/STATUS.md`.
4. Resolve routine engineering choices autonomously using the defaults and decision rules in this file. Ask the user only when a genuine external decision is required (for example, a paid service, destructive operation, private credential, hardware/BIOS change, licence incompatibility, or a product choice that cannot be inferred safely).
5. Implement the highest-priority unfinished task, not merely describe it.
6. Build the smallest affected target; run relevant unit/integration tests; diagnose failures; fix the underlying cause; rerun the failing test and the broader regression set.
7. Continue to the next task when the relevant gates are green. Do not stop after a single successful compile, a mock test, a TODO list, a UI prototype or a README update.
8. Keep a factual engineering log with exact commands, commit hashes, tool versions, model hashes, test results and measured performance. Do not fabricate test results or claim that hardware was tested when it was not.
9. Update documentation and examples in the same change as the implementation.
10. At the end, produce a release-readiness report that distinguishes **PASS**, **FAIL**, **BLOCKED** and **NOT RUN**. A blocked gate is not a pass.

### 0.2 Autonomous work policy

- **Default to action, not questions.** If a safe, reversible decision can be made from this specification, make it and document it in an Architecture Decision Record (ADR).
- **No fake completion.** A placeholder endpoint, stubbed engine, fake benchmark, hard-coded hardware report or mocked inference is not a completed feature.
- **No silent feature removal.** Do not delete failing tests, weaken assertions, lower validation coverage or disable a failing backend simply to obtain a green build. An intentional feature-flagged fallback must be documented and tested.
- **No arbitrary upgrade.** Pin upstream dependencies to exact commits. Update them in separate compatibility changes, not opportunistically during unrelated fixes.
- **No blind rewrite.** Preserve upstream code where practical. Keep patches small, explain why each patch is needed, and maintain a patch series or fork commit history.
- **No unsupported performance claims.** Report measured values with the exact machine, driver, build, model, quantization, context, input, thermal state and repetitions.
- **No unsafe “optimization”.** Never disable Windows security, the page file, antivirus, GPU driver protections or validation globally to improve a benchmark. Never modify BIOS settings automatically.
- **No destructive storage operation.** Do not format, repartition, delete, rewrite or benchmark-destructively fill a user's HDD. Use non-destructive read-only benchmarks by default. Any write benchmark must use a newly created test file in an explicitly chosen scratch directory and remove only that file after user-safe validation.
- **No automatic external exposure.** API and MCP bind to loopback by default. LAN exposure is an explicit opt-in with authentication and origin validation.
- **No secrets in source control.** Do not commit API tokens, private model links, user data, telemetry dumps, full prompts or personal images.

### 0.3 Self-healing loop contract

Every implementation task follows this loop:

```text
INSPECT
  -> DEFINE ACCEPTANCE CRITERIA
  -> REPRODUCE / ESTABLISH BASELINE
  -> IMPLEMENT THE SMALLEST CHANGE
  -> FORMAT + STATIC ANALYSIS
  -> CONFIGURE + BUILD
  -> RUN TARGETED TESTS
  -> RUN REGRESSION TESTS
  -> RUN SECURITY / RESOURCE CHECKS
  -> BENCHMARK IF PERFORMANCE-RELATED
  -> UPDATE DOCS + EVIDENCE
  -> COMMIT OR MOVE TO NEXT TASK
```

When a gate fails:

```text
FAIL
  -> CAPTURE FULL LOG / TEST CASE / ENVIRONMENT
  -> CLASSIFY ROOT CAUSE
       [code | configuration | dependency | driver | hardware | model | environment]
  -> FORM A TESTABLE HYPOTHESIS
  -> APPLY ONE ISOLATED FIX
  -> REBUILD THE SMALLEST AFFECTED TARGET
  -> RERUN THE EXACT FAILURE
  -> RERUN RELATED REGRESSIONS
  -> KEEP FIX ONLY IF IT IMPROVES THE EVIDENCE
  -> OTHERWISE REVERT THE EXPERIMENT AND TRY A DIFFERENT HYPOTHESIS
```

Loop limits are a protection against wasting time, not a reason to stop engineering:

- Try up to 3 materially different root-cause hypotheses for one failure before widening the investigation.
- After 3 failed hypotheses, inspect upstream issues, source, compiler output and dependency history; create a minimal reproducer.
- Do not repeat an identical failed command without changing a relevant input.
- If a failure depends on unavailable real hardware or model weights, create a documented test fixture where possible, mark the real-hardware gate **BLOCKED**, and continue with all independent tasks.
- If a toolchain installation or download is needed, use official release sources in §6. Do not install random binaries or execute unverified scripts.
- Continue until all tasks possible in the current environment are completed. Never claim that work will continue in the background after the session.

### 0.4 Definition of evidence

A claim is considered proven only when there is a reproducible artefact or test record. Evidence should include:

- command line and working directory;
- compiler, CMake, generator, SDK and dependency versions;
- source commit SHA and dependency commit SHAs;
- exact test name and result;
- device/driver/Vulkan information for GPU tests;
- model hash and model licence/source for model tests;
- machine-readable benchmark result (JSON/CSV) plus a short human-readable summary;
- logs attached or stored under `artifacts/` with sensitive data redacted.

A green GitHub Actions build is not proof of Radeon 680M runtime correctness. A CPU smoke test is not proof of Vulkan. A simulated model is not a real model test. A GUI screenshot is not proof that the API works.

---

## 1. Product definition, scope and non-goals

### 1.1 V1 product

HCSInfer is a local-first Windows application that provides:

1. **Chat:** GGUF text inference through `llama.cpp`.
2. **Vision:** supported image understanding through `llama.cpp` multimodal components (`libmtmd`) and the relevant model/projector files.
3. **Image Studio:** text-to-image and supported image-to-image/edit workflows through `stable-diffusion.cpp`.
4. **Model Manager:** model scanning, metadata validation, model selection, loading, unloading and storage-root management.
5. **Performance Center:** hardware diagnostics, memory status, backend state, load time, TTFT, tokens/s, image runtime and I/O metrics.
6. **Unified Control Plane:** model registry, engine lifecycle, global resource budget, job queue, cancellation, logs, settings and telemetry.
7. **CLI:** reliable commands for diagnostics, model management, inference, server, MCP and benchmarks.
8. **HTTP API:** documented OpenAI-compatible endpoints where semantic compatibility is genuine, plus HCS-native endpoints.
9. **MCP:** local stdio server first; optional Streamable HTTP with the protocol version actually implemented and tested.
10. **Model Vault:** use the external HDD as model archive and source; bounded asynchronous read/prefetch into RAM; no HCS-owned decompressed model cache or staging file on the SSD by default.
11. **Compute:** upstream CPU and Vulkan backends plus opt-in HCS Vulkan kernels that are numerically validated and measurably beneficial.
12. **Packaging:** target a single `HCSInfer.exe` for Windows when static integration and licence compatibility are proven. During development, a portable folder build is acceptable and preferable to a fragile one-file build.

“All-in-one” means one application, model registry, settings set, lifecycle manager, scheduler, API and MCP entry point. It does not mean one neural network architecture handles all text, image understanding and image synthesis.

### 1.2 V1 non-goals

Do not make these prerequisites for V1:

- writing a new Windows or Linux graphics driver;
- replacing AMD's Vulkan driver or the Windows Vulkan loader;
- reimplementing a transformer or diffusion framework from scratch;
- promising that a mechanical USB HDD performs like RAM or VRAM;
- including multi-gigabyte model weights inside the executable;
- requiring Docker, WSL, cloud accounts or hosted inference;
- enabling ROCm as the baseline for this 680M target;
- supporting every model family or every quantization format;
- enabling an experimental HCS kernel merely because it compiles;
- promising universal performance improvement across every AMD GPU.

### 1.3 Target hardware reality

The target AMD Ryzen 7 7735HS is an 8-core/16-thread Zen 3+ CPU with AVX2 and FMA3 and a Radeon 680M iGPU (RDNA 2, 12 CUs / 768 shader cores). The iGPU uses system memory. CPU and GPU compete for the system's memory bandwidth and capacity.

Required design consequences:

- Detect actual installed RAM and channel configuration where possible; do not assume the user has a particular capacity from the product listing.
- Detect actual UMA reservation and Vulkan memory heaps. A BIOS-reserved framebuffer is not extra physical RAM.
- Query Vulkan features/extensions at runtime; never infer feature support from the GPU name alone.
- Avoid static assumptions such as “8 GB VRAM”.
- Keep a CPU-only fallback and clear diagnostics for Vulkan failure.
- Do not assume CPU+GPU execution is faster than either alone. Transfer, synchronisation, dispatch and shared-bandwidth contention can erase gains.
- Measure on the user's actual driver and memory configuration.
- Treat ROCm as optional future research, not a requirement for Radeon 680M V1. Check the current AMD compatibility list before claiming support.

### 1.4 Product-quality principle

The default experience must be simple for a non-expert. Advanced controls are available but not forced on normal users. Every advanced option must include a safe default, a concise explanation, validation and a reset path.

---

## 2. Repository reconnaissance and preservation

Before modifying anything:

1. Run `git status --short --branch`.
2. Run `git log -8 --oneline --decorate`.
3. Inspect top-level files, current CMake files, source tree, workflows, docs and tests.
4. Identify current language/toolchain, existing project conventions and user modifications.
5. Inspect `.gitmodules`, package manifests, dependency locks, build scripts and release history.
6. Record the actual current state in `docs/roadmap/STATUS.md`.
7. If repository contents differ from the suggested structure, adapt the structure; do not blindly replace an existing working project.

Preservation rules:

- Never run `git clean -xfd`, `git reset --hard`, `git checkout -- .` or destructive equivalent.
- Never rewrite user-authored files in bulk without inspecting their contents.
- Prefer focused patches and small commits.
- Keep generated build files outside source-controlled directories where practical (`build/`, `out/`, `cmake-build-*`).
- Do not commit model weights, build output, user logs, secrets or local machine paths.
- Use `.gitignore` to exclude generated artefacts and local model folders.

If no repository code exists yet, scaffold only the minimum structure needed for Milestone 0 and keep the full planned architecture documented rather than filling the tree with empty placeholder source files.

---

## 3. Architecture and dependency boundaries

### 3.1 High-level architecture

```text
HCSInfer.exe
│
├── UI Shell
│   ├── Chat / Vision / Image Studio
│   ├── Model Manager
│   ├── Performance Center
│   └── Settings
│
├── CLI Router
│
├── Control Plane
│   ├── Model Registry
│   ├── Job Queue / Cancellation
│   ├── Model Lifecycle
│   ├── Memory Budget Manager
│   ├── CPU/GPU Scheduler
│   ├── Settings / Profiles
│   └── Telemetry / Structured Logs
│
├── Unified API Gateway
│   ├── OpenAI-compatible chat / image routes
│   └── HCS-native health / hardware / memory / jobs routes
│
├── MCP Server
│   ├── stdio
│   └── optional Streamable HTTP
│
├── Engine Adapters (narrow stable interfaces)
│   ├── Text/Vision Adapter -> llama.cpp + GGML + libmtmd
│   └── Diffusion Adapter -> stable-diffusion.cpp + GGML
│
├── Runtime Services
│   ├── Hardware Detector
│   ├── Memory / Buffer Manager
│   ├── HCS Model Vault
│   ├── Vulkan Device Manager
│   ├── Kernel Registry / Profiler / Autotuner
│   ├── CPU AVX2/FMA Executor
│   └── Resource / Job Scheduler
│
└── Compute Backends
    ├── GGML CPU backend
    ├── GGML Vulkan backend
    └── HCS experimental Vulkan kernels
```

### 3.2 Stable HCS-owned interfaces

- GUI, CLI, API and MCP must call the HCS control-plane services, never raw `llama_*` or `sd_*` functions.
- Upstream-specific code belongs only inside engine adapters and minimal compatibility shims.
- Do not expose upstream C++ classes in public HCS headers.
- Prefer narrow value types and an opaque C ABI at engine boundaries if this simplifies static linking and prevents ABI leakage.
- All asynchronous operations return a job identifier and support status, cancellation and structured errors.
- Model operations must be serialized or explicitly concurrency-safe. Do not allow two callers to race load/unload on the same engine context.
- Use RAII for every C++ resource and explicit ownership for Vulkan objects.
- Define thread ownership for Vulkan queues, command pools, model contexts and callbacks.

Suggested interfaces (adapt to existing code; not a mandate to use these exact names):

```cpp
struct ModelInfo;
struct LoadRequest;
struct TextRequest;
struct VisionRequest;
struct ImageRequest;
struct JobId;
struct EngineMetrics;

class ITextEngine {
public:
    virtual Result<void> load(const LoadRequest&) = 0;
    virtual Result<void> unload() = 0;
    virtual Result<JobId> generate(const TextRequest&) = 0;
    virtual Result<JobId> analyze_image(const VisionRequest&) = 0;
    virtual EngineMetrics metrics() const = 0;
    virtual ~ITextEngine() = default;
};

class IImageEngine {
public:
    virtual Result<void> load(const LoadRequest&) = 0;
    virtual Result<void> unload() = 0;
    virtual Result<JobId> generate(const ImageRequest&) = 0;
    virtual Result<JobId> edit(const ImageRequest&) = 0;
    virtual EngineMetrics metrics() const = 0;
    virtual ~IImageEngine() = default;
};
```

### 3.3 GGML integration risk is a mandatory proof gate

`llama.cpp` uses upstream GGML. `stable-diffusion.cpp` currently defaults to a patched GGML submodule with additional operators/extensions. Its current build documentation describes `SD_USE_UPSTREAM_GGML=ON` and `SD_GGML_SOURCE_DIR` for using an upstream GGML tree, but documents limitations: some patched operators/optimisations are absent; certain INT8 paths and FP8 weight paths are disabled/rejected or converted differently.

Never assume that both trees can be added to one CMake project without duplicate targets, symbols or ABI conflicts.

Test in this order:

1. Build both upstream repositories separately at exact pinned commits.
2. Record each repository's GGML commit and build options.
3. Test `stable-diffusion.cpp` in upstream-GGML mode against representative diffusion models.
4. Compare model/operator coverage and numerical results with its patched-GGML mode.
5. Only then attempt one shared GGML build in a combined CMake graph.
6. Verify all text, vision and diffusion smoke tests after combining.
7. If incompatible, use clean adapter boundaries and separate internal engine libraries or a development multi-process mode; do not corrupt either upstream.
8. The single-EXE release gate remains blocked until one-process linking and clean-machine tests prove it.

Do not silently link two incompatible GGML versions into one process. Record exact submodule commit SHAs and patch revisions in `external/dependency-lock.json` (or an equivalent machine-readable manifest).

### 3.4 C++ standard boundaries

HCSInfer-owned code should target C++20. Do not globally force all third-party targets to C++20 if upstream projects require a different standard. Keep target-level compile features and follow the exact upstream build configuration. Avoid changing third-party compiler flags globally.

---

## 4. Required repository structure and documentation

Adapt to existing conventions, but ensure these responsibilities exist:

```text
HCSInfer/
├── AGENTS.md
├── README.md
├── CONTRIBUTING.md
├── SECURITY.md
├── LICENSE
├── THIRD_PARTY_NOTICES.md
├── CMakeLists.txt
├── CMakePresets.json
├── .gitmodules
├── .gitignore
├── external/
│   ├── llama.cpp/                 # pinned submodule
│   └── stable-diffusion.cpp/      # pinned submodule
├── src/
│   ├── app/                       # entry point / mode router
│   ├── cli/
│   ├── gui/
│   ├── core/                      # lifecycle, jobs, settings
│   ├── hardware/                  # CPU / Vulkan / device detection
│   ├── memory/                    # budget, pools, residency
│   ├── storage/                   # HDD IO, chunks, codecs
│   ├── scheduler/
│   ├── api/
│   ├── mcp/
│   ├── engines/llama/
│   ├── engines/diffusion/
│   ├── kernels/cpu/
│   ├── kernels/vulkan/
│   └── telemetry/
├── shaders/
│   ├── common/
│   ├── llm/
│   └── diffusion/
├── tests/
│   ├── unit/
│   ├── contract/
│   ├── parser-fuzz/
│   ├── kernels/
│   ├── integration/
│   ├── e2e/
│   ├── performance/
│   └── soak/
├── scripts/
│   ├── bootstrap-windows.ps1
│   ├── configure.ps1
│   ├── build.ps1
│   ├── test.ps1
│   ├── benchmark.ps1
│   ├── doctor.ps1
│   └── package.ps1
├── tools/
│   ├── benchmark-runner/
│   └── model-manifest-validator/
├── packaging/
│   ├── windows/
│   └── linux/
├── docs/
│   ├── architecture.md
│   ├── hardware-support.md
│   ├── model-vault.md
│   ├── vulkan-kernels.md
│   ├── api.md
│   ├── mcp.md
│   ├── benchmarks.md
│   ├── supported-models.md
│   ├── roadmap/
│   │   ├── IMPLEMENTATION_PLAN.md
│   │   └── STATUS.md
│   ├── decisions/ADR-*.md
│   ├── research/
│   └── test-evidence/
└── artifacts/                     # ignored; local CI evidence only
```

### 4.1 README requirements

The README must be useful to a first-time user and technically honest. Include:

- one-paragraph product summary and current status (prototype/alpha/beta/stable);
- truthful feature matrix with `Implemented`, `Experimental`, `Planned`, `Unsupported` states;
- actual supported hardware, OS and Vulkan driver requirements;
- current limitations for Radeon 680M and shared memory;
- screenshots only of real working features;
- exact Windows prerequisites and download links;
- quick start for build and run;
- model import instructions and model-license reminder;
- CLI examples copied from tests;
- API and MCP configuration examples;
- Model Vault path configuration and no-SSD-cache guarantee scope;
- reproducible benchmark methodology and latest verified results;
- troubleshooting (Vulkan missing, model unsupported, OOM, device lost, external HDD disconnected);
- known issues;
- security and privacy explanation;
- development/build/test instructions;
- architecture and research links;
- licence and third-party notice links.

Do not advertise a feature until its corresponding acceptance test passes. Mark planned features clearly. No fake benchmark badges, invented users, fake screenshots or “SOTA” claims without measured evidence.

### 4.2 Maintain architecture decision records

Create ADRs for decisions with long-term implications, at least:

- ADR-001: engine integration and GGML topology;
- ADR-002: one-process vs development process isolation;
- ADR-003: Vulkan queue/device ownership;
- ADR-004: memory budget and UMA accounting;
- ADR-005: Model Vault and no-SSD app cache policy;
- ADR-006: chunk container/version/checksum design;
- ADR-007: API and MCP security boundaries;
- ADR-008: single-EXE static linking and CRT strategy;
- ADR-009: GUI toolkit and renderer;
- ADR-010: benchmark methodology and performance regression thresholds.

Each ADR includes context, options considered, decision, rejected alternatives, risks, evidence, migration and date.

---

## 5. Development milestones and execution order

Do not jump to custom kernels or final packaging before the upstream runtime and hardware baselines are proven.

### M0 — Repository and hardware baseline

Deliver:

- repository status and dependency lock;
- `HCSInfer doctor` prototype;
- CPU feature report;
- Vulkan device/feature/heap report;
- Windows build/environment report;
- HDD identity and non-destructive read-speed benchmark;
- initial CPU and Vulkan engine build logs.

Acceptance:

- target CPU/iGPU identified from actual hardware query;
- Vulkan device enumeration is truthful;
- CPU fallback works when Vulkan is disabled;
- detected values are exported to JSON;
- report includes Windows build, AMD driver, Vulkan API/driver, RAM, BIOS UMA where detectable and external drive filesystem/interface if detectable;
- unknown values are reported as `unknown`, not guessed.

### M1 — independent engine proof

- clone/pin/build `llama.cpp` separately with Vulkan;
- run its device listing;
- perform real text generation with an actual GGUF from the external drive;
- run real multimodal image analysis using a supported model/projector;
- clone/pin/build `stable-diffusion.cpp` separately with Vulkan;
- run one real low-resolution diffusion smoke test;
- save exact commands, commit SHAs, model hashes, output and measurements.

Acceptance:

- all three workloads (text, vision, diffusion) work outside the HCSInfer code;
- CPU-only fallback is verified;
- CPU/Vulkan baselines are separately recorded;
- unsupported model/feature behavior is clear and does not crash.

### M2 — headless HCSInfer core

Implement:

- C++20 application core;
- `ITextEngine` and `IImageEngine` adapters;
- model registry and manifest;
- model load/unload state machine;
- single global memory budget;
- job queue, streaming events and cancellation;
- structured logs and error taxonomy;
- simple `doctor`, `models`, `chat`, `vision`, `image` CLI commands.

Acceptance:

- all engine operations route through adapters;
- no GUI/API path calls upstream internals directly;
- model switching frees the previous context safely;
- cancellation does not leak model, Vulkan or file resources;
- CPU fallback remains available.

### M3 — unified API and MCP

Implement:

- OpenAI-compatible text chat endpoint;
- supported image understanding input on chat endpoint;
- image generation/edit endpoints where the request semantics match;
- HCS health, runtime, devices, memory, models and jobs endpoints;
- job status and cancellation;
- MCP stdio tools with narrow schemas;
- optional Streamable HTTP only after security and protocol conformance tests.

Acceptance:

- real HTTP client call produces inference;
- streaming response is tested;
- cancellation stops actual work;
- malformed/oversized request is rejected;
- no arbitrary shell execution or unapproved path access exists;
- API binds to `127.0.0.1` by default;
- MCP tools are exercised by a real MCP client.

### M4 — GUI / normal-user profiles

Implement:

- Chat;
- Vision;
- Image Studio;
- Models;
- Performance Center;
- Settings;
- Simple and Advanced modes;
- clear busy/loading/error/unsupported/OOM states.

Acceptance:

- a normal user can load a supported model, chat, analyse an image and generate an image without a terminal;
- GUI shows actual engine state and hardware selection;
- controls are wired to real runtime functions;
- no inert buttons or fake progress indicators;
- model load and job cancellation are functional.

### M5 — Model Vault v1 (original formats first)

Implement:

- approved HDD root selection;
- scan/manifest/metadata/hash validation;
- asynchronous sequential reads;
- bounded RAM buffers and prefetch;
- load modes based on actual backend capabilities;
- no HCS-owned decompressed model cache on SSD by default;
- explicit memory and I/O telemetry.

Acceptance:

- real model loads from external HDD;
- peak RAM is bounded and measured;
- USB disconnect is handled without crash/corruption;
- the application never creates a decompressed model staging file on SSD unless user explicitly configures an exception;
- cold and warm load are separately benchmarked;
- documentation accurately states that Windows may page memory to its system page file.

### M6 — HCSMODEL experimental chunk container

Implement only after Model Vault v1 works:

- versioned header and chunk table;
- 64-bit offsets/sizes with overflow-safe arithmetic;
- independent chunks;
- Raw, LZ4 and Zstandard codecs;
- per-chunk integrity hash/checksum and manifest hash;
- streaming decode into bounded RAM buffers;
- chunk sizes benchmarked (start with 4–64 MiB candidates);
- raw storage if compression does not exceed a configured net-benefit threshold.

Acceptance:

- bit-identical decompression for all supported data;
- parser fuzzing and malformed-file tests pass;
- random access to a chunk works without decoding the entire container;
- measured load/throughput improvement exists in at least one real HDD scenario;
- no blanket claim that all quantized GGUF weights compress well.

### M7 — shared GGML ABI and single-EXE proof

- validate `SD_USE_UPSTREAM_GGML=ON` against pinned upstream GGML;
- run diffusion model coverage tests and compare patched mode;
- resolve target/symbol conflicts;
- statically link HCSInfer and both engine adapters where licensing permits;
- embed shader bytecode and small assets;
- package one EXE;
- test on a clean Windows x64 environment with no development SDK installed.

Acceptance:

- one HCSInfer.exe is sufficient for application code (models and system GPU driver remain external);
- no unexpected sidecar DLL or child process is required;
- Vulkan startup works with the installed driver;
- CPU fallback works with Vulkan unavailable;
- licence notices and dependency report are complete;
- if common GGML is not compatible, do not claim one-EXE stable V1; retain a transparent portable dev package.

### M8 — selective HCS Vulkan kernels

Profile existing upstream operators first. Implement one candidate at a time and keep it behind a feature flag.

Suggested order:

1. quantized GEMV (decode);
2. quantized GEMM (prefill);
3. fused dequantization + matrix operation;
4. RMSNorm/residual fusion;
5. RoPE;
6. SwiGLU/elementwise fusion;
7. Attention/KV cache hot paths;
8. diffusion convolution/GEMM hot paths;
9. tiled attention and VAE optimisations.

Acceptance per kernel:

- independent CPU/reference implementation;
- upstream Vulkan baseline;
- shape/dtype/stride/quantisation coverage;
- numerical error bounds;
- Vulkan validation and SPIR-V validation;
- stable benchmark with warm-up and repeated runs;
- dispatch falls back if the shape or feature is unsupported;
- default-enable only if a repeatable benefit is measured on supported hardware without unacceptable accuracy regression.

### M9 — advanced research features

Only after baseline V1 is stable:

- paged/block-based KV cache improvements;
- quantized KV cache profile tuning;
- FlashAttention-inspired tiled/IO-aware attention;
- adaptive per-op CPU/GPU scheduling;
- speculative decoding options supported upstream;
- dynamic diffusion tiling and memory-aware graph execution;
- additional model-format or backend support.

Every feature requires its own research note, baseline, implementation flag, correctness test, performance test, quality assessment and documented fallback.

### M10 — V1 hardening and release

- security and dependency audit;
- real-model soak tests;
- model and parser fuzzing;
- memory leak/resource lifecycle tests;
- OOM/device-lost/HDD-unplug recovery;
- clean-machine tests;
- static/dynamic dependency and licence audit;
- SBOM and checksums;
- signed release build when signing credentials are available;
- release notes and support matrix;
- reproducible benchmark artefacts;
- README and docs match actual functionality.

---

## 6. Build environment, downloads and official sources

### 6.1 Windows development prerequisites

Use official sources; pin tool versions in CI and record local versions in `artifacts/environment.json`.

| Tool | Purpose | Official download / docs |
|---|---|---|
| Git for Windows | Source control / submodules | https://git-scm.com/download/win |
| Visual Studio 2022 Community or Build Tools | MSVC x64 compiler, Windows SDK, CMake integration | https://visualstudio.microsoft.com/vs/community/ and https://visualstudio.microsoft.com/downloads/ |
| C++ workload | Select “Desktop development with C++”, MSVC v143, Windows 10/11 SDK, CMake tools | https://learn.microsoft.com/en-us/cpp/build/vscpp-step-0-installation |
| CMake | Configure generator/build | https://cmake.org/download/ |
| Ninja (optional but recommended) | Fast single-config builds | https://github.com/ninja-build/ninja/releases |
| Vulkan SDK (build-time) | Vulkan headers/libs, shader compiler, SPIR-V tools, validation layers | https://vulkan.lunarg.com/sdk/home |
| Python 3 (developer tooling only) | Benchmark/report scripts where needed | https://www.python.org/downloads/windows/ |
| 7-Zip (optional) | Inspect archives | https://www.7-zip.org/ |

The LunarG download page is authoritative for SDK releases and published SHA-256 hashes. At the time this specification was prepared, the page listed Vulkan SDK 1.4.363.0 for Windows x64 (29 September 2026). Do not hard-code that version forever: check the current download page, select a stable release, record its exact version/hash and keep CI pinned. The Vulkan SDK is a build/development dependency, not an end-user runtime requirement.

**Runtime distinction:** The installed AMD/OEM graphics driver and Vulkan ICD are system components. HCSInfer must query them at startup. A bundled SDK does not replace a graphics driver.

### 6.2 AMD driver and diagnostics

- Prefer the mini-PC/OEM validated graphics driver if OEM documentation recommends it.
- AMD's official 7735HS driver page is: https://www.amd.com/en/support/downloads/drivers.html/processors/ryzen/ryzen-7000-series/amd-ryzen-7-7735hs.html
- Check AMD release notes and the OEM support page before updating a working driver.
- Create a restore point or otherwise preserve a known-good driver before a change.
- Use `vulkaninfo --summary` (from SDK) to record instance/device/driver information.
- Use `vkcube` only as a basic presentation/driver smoke test, not proof of compute correctness.
- Use the `llama.cpp` device listing and a real inference test to prove GGML Vulkan enumeration and execution.

### 6.3 Vulkan SDK validation tools

The SDK should supply relevant tools such as `glslc`, `spirv-val`, `spirv-dis`, `vulkaninfo` and validation layers, depending on selected components/version.

Use:

```powershell
$env:VULKAN_SDK
Get-Command glslc, spirv-val, vulkaninfo -ErrorAction SilentlyContinue
vulkaninfo --summary
```

Do not assume every executable is on `PATH`; locate it under `$env:VULKAN_SDK\Bin` if needed. Record exact output.

### 6.4 Linux later

For the Linux target, use a supported distribution, Mesa RADV and AMDGPU kernel driver. Official docs:

- Mesa RADV: https://docs.mesa3d.org/drivers/radv.html
- Mesa project: https://gitlab.freedesktop.org/mesa/mesa
- Vulkan SDK Linux: https://vulkan.lunarg.com/sdk/home

Linux is not a reason to block Windows V1. Keep OS-specific I/O and packaging behind interfaces.

---

## 7. Dependency acquisition, pinning and supply-chain controls

### 7.1 Required upstream repositories

- llama.cpp: https://github.com/ggml-org/llama.cpp
- stable-diffusion.cpp: https://github.com/leejet/stable-diffusion.cpp
- GGML is included in upstream repositories; maintain explicit commit provenance.

Use Git submodules or a documented equivalent. Prefer submodules for reproducible source tracking unless the repository already has a working dependency manager.

Example first-time checkout:

```powershell
git submodule update --init --recursive
```

For a new checkout, add only after recording the repository's current conventions:

```powershell
git submodule add https://github.com/ggml-org/llama.cpp external/llama.cpp
git submodule add https://github.com/leejet/stable-diffusion.cpp external/stable-diffusion.cpp
git submodule update --init --recursive
```

Do not run `git submodule add` if the project already manages those sources another way.

### 7.2 Pinning policy

For every dependency record:

- canonical URL;
- exact commit SHA/tag;
- licence identifier and source file;
- local patches and their reason;
- CMake options;
- compiler/CRT assumptions;
- tested platform and build result;
- known incompatibilities.

Never use a floating `master`/`main` dependency in a release build. A bot may notify about upstream changes, but only an explicit compatibility PR can update the pin.

### 7.3 Licence and model provenance

- Inspect the licence for every bundled code dependency and transitive dependency.
- Respect upstream attribution and source-distribution requirements.
- Do not assume that a code licence grants rights to distribute model weights.
- Keep models outside the repository and EXE.
- Provide model cards and links to the original model licence.
- Never redistribute model weights unless the licence and distribution terms explicitly allow it.
- Generate `THIRD_PARTY_NOTICES.md` and an SBOM for releases.

---

## 8. Baseline builds: build both upstream engines separately first

Use a Visual Studio Developer PowerShell or a shell with the MSVC x64 environment correctly initialized. Always configure separate build directories for distinct configurations/generators. Do not reuse a CMake cache after changing compiler, generator, SDK or major build options.

### 8.1 Record tool versions

```powershell
cmake --version
git --version
cl
ninja --version
$env:VULKAN_SDK
vulkaninfo --summary
```

Capture the output in `artifacts/environment.txt` (ignored by Git).

### 8.2 Build llama.cpp Vulkan baseline

The current upstream Windows/Vulkan instructions use CMake and `GGML_VULKAN=ON`.

Visual Studio generator:

```powershell
cmake -S external/llama.cpp -B build/llama-vulkan -G "Visual Studio 17 2022" -A x64 `
  -DGGML_VULKAN=ON `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/llama-vulkan --config Release --parallel
```

Ninja alternative (only if the MSVC environment is active):

```powershell
cmake -S external/llama.cpp -B build/llama-vulkan-ninja -G Ninja `
  -DGGML_VULKAN=ON `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/llama-vulkan-ninja --parallel
```

The exact output binary path can vary by generator/configuration. Discover it instead of assuming it. Then:

```powershell
# Replace with the actual resolved path from the current build.
& $LlamaCli --help
& $LlamaCli --list-devices
```

Run a real model smoke test using a model path supplied in the local test configuration:

```powershell
& $LlamaCli -m $env:HCS_TEST_GGUF -p "Reply with exactly: HCSInfer smoke test passed." `
  -n 32 -ngl 99 --seed 42
```

If a parameter differs at the pinned upstream commit, check `--help` and the pinned documentation. Do not modify commands blindly or assert that GPU execution occurred merely because the CLI returned text. The logs must identify the Vulkan backend/device and report layer offload or operation placement.

### 8.3 Build stable-diffusion.cpp Vulkan baseline

For initial independent model validation, keep examples enabled so CLI/server test binaries are available:

```powershell
cmake -S external/stable-diffusion.cpp -B build/sd-vulkan -G "Visual Studio 17 2022" -A x64 `
  -DSD_VULKAN=ON `
  -DSD_BUILD_EXAMPLES=ON `
  -DSD_BUILD_SHARED_LIBS=OFF `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/sd-vulkan --config Release --parallel
```

If the selected upstream revision does not define one of these options, inspect that revision's `CMakeLists.txt` and build guide and use the real supported option. Never quietly ignore an unknown option warning.

Use the pinned CLI's own `--help` and model examples. A backend placement smoke-test shape is:

```powershell
# Adapt model-specific flags to the supported architecture and exact CLI help.
& $SdCli -m $env:HCS_TEST_DIFFUSION_MODEL -p "a simple red apple on a plain table" `
  --backend vulkan0 --params-backend cpu
```

Then test disk parameter placement as a separate experiment:

```powershell
& $SdCli -m $env:HCS_TEST_DIFFUSION_MODEL -p "a simple red apple on a plain table" `
  --backend vulkan0 --params-backend disk
```

The `disk` parameter backend is a memory-capacity mode, not a promise of faster inference. It may re-read weights and be slower. Test CPU parameters first, then disk parameters, and record per-module placement. Do not assume the actual runtime backend name is `vulkan0` without checking the CLI device list.

### 8.4 Multimodal validation

- Follow the current llama.cpp `libmtmd` developer guide.
- Use a model family explicitly supported by the pinned revision.
- Download the model and any projector/mmproj file from a legitimate model repository whose licence allows the intended test.
- Verify all file hashes and record model-card URL/licence.
- Use one small, fixed test image with redistribution rights.
- Run through upstream CLI/server before integrating it into HCSInfer.
- A plain text GGUF model is not automatically a VLM. A missing projector or unsupported model is a clear compatibility error, not a reason to fake success.

---

## 9. HCSInfer build system and command contract

### 9.1 CMake expectations

Create `CMakePresets.json` with named presets such as:

- `windows-msvc-debug`;
- `windows-msvc-release`;
- `windows-msvc-release-vulkan`;
- `windows-clang-asan` (only where supported);
- `linux-clang-debug`;
- `linux-release-vulkan`.

Presets must record generator, architecture, build type, toolchain and relevant cache variables. Do not check in machine-specific absolute paths.

Use CMake target-level options. Enable `CMAKE_EXPORT_COMPILE_COMMANDS` where supported. Use `CTest` and a test framework such as GoogleTest (pinned) for C++ tests.

### 9.2 Baseline configure/build/test commands

After the project CMake is in place:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --parallel
ctest --preset windows-msvc-debug --output-on-failure
```

For release/Vulkan:

```powershell
cmake --preset windows-msvc-release-vulkan
cmake --build --preset windows-msvc-release-vulkan --parallel
ctest --preset windows-msvc-release-vulkan --output-on-failure
```

If preset-based build/test is not yet implemented, provide equivalent scripts and commands. Ensure `ctest -N` lists expected tests and `ctest --output-on-failure` returns a nonzero exit code when tests fail.

### 9.3 Build scripts

Create PowerShell scripts with strict error handling:

```powershell
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
```

`scripts/build.ps1` should support:

- `-Preset`;
- `-CleanBuildDir` (only remove the designated build directory after validating its resolved path is under the repository build root);
- `-Parallel`;
- `-RunTests`;
- `-Vulkan`;
- `-Verbose`.

`scripts/test.ps1` should support test labels/categories, keep logs, and return nonzero on any failure.

`scripts/doctor.ps1` should collect diagnostics without changing system settings.

Never let scripts remove arbitrary paths based on user input without strict canonical path checks.

### 9.4 Compiler and warning policy

- HCS-owned code: warnings enabled; fix warnings introduced by HCS code.
- Third-party upstream targets: do not impose project warning-as-error flags globally if they produce unrelated upstream build failures.
- Use `/W4` or an agreed strict MSVC warning profile; selectively enable `/WX` for HCS targets after baseline is clean.
- Run `clang-tidy` on HCS-owned sources where the compile database is available.
- Run `clang-format --dry-run --Werror` (or project formatter) in CI.
- Use static analysis in a dedicated CI job; do not treat the absence of a tool as success.

### 9.5 Static CRT and one-file target

The final Windows packaging may use MSVC static CRT (`/MT`) if every linked dependency and upstream target is built with a compatible runtime. Use CMake policy `CMP0091` and `CMAKE_MSVC_RUNTIME_LIBRARY` consistently, for example a `MultiThreaded` runtime for Release and `MultiThreadedDebug` for Debug, but do not force this until all subprojects are compatible.

The single-EXE objective includes HCSInfer code, GUI, CLI, API, MCP stdio, engine adapters, static libraries and embedded SPIR-V/assets where legal. It does not include model weights, AMD graphics driver, Windows system libraries or the Vulkan ICD. Do not bundle or replace the system driver.

---

## 10. Runtime memory, UMA and model lifecycle

### 10.1 One global memory budget

Create a budget model from live measurements, not static constants:

```text
Available application budget
  = min(
      current available physical RAM - OS reserve - safety margin,
      supported Vulkan memory budget/headroom where available
    )
```

Account separately for:

- host model weights;
- Vulkan buffers and UMA allocations;
- KV cache;
- graph workspace and temporary tensors;
- VLM projector/vision buffers and decoded pixels;
- diffusion text encoder, UNet/DiT, VAE and latents;
- GUI/API and runtime overhead;
- prefetch/decompression buffers.

No double counting: a shared physical allocation must not be counted as independent RAM plus dedicated VRAM. Document what each Windows/Vulkan counter actually means.

### 10.2 User profiles

Provide:

- `Auto`: measured capabilities, conservative selection;
- `Fast`: keep active working set resident where possible;
- `Balanced`: preserve system headroom and unload inactive engines;
- `Low Memory`: smaller context/batch/resolution and aggressive release;
- `CPU Only`: no Vulkan inference.

No automatic BIOS, clock or power-plan modification.

### 10.3 Lifecycle state machine

Model lifecycle must have explicit states, for example:

```text
Unregistered -> Discovered -> Validating -> ReadyToLoad
  -> Loading -> Loaded -> Running -> Unloading -> Unloaded
                    \-> Failed (recoverable / non-recoverable)
```

Rules:

- only one owner may mutate a model's lifecycle state at a time;
- every allocation has a matching release path;
- load failure rolls back partial allocations;
- unload waits for or cancels active jobs safely;
- device-lost and OOM paths release resources where Vulkan permits and mark the device/context unusable until recreated;
- model switch checks memory budget before evicting the current model;
- user receives a reason and suggested remediation (smaller quant, shorter context, CPU mode, unload previous model).

### 10.4 KV cache

Use llama.cpp's supported cache types and options before designing a new cache implementation. Current server docs list multiple K/V cache data types; exact support depends on the backend and model architecture.

Test F16, Q8 and Q4 cache types where the pinned backend supports them. Measure:

- peak memory at 4K/8K/16K/32K contexts where the model supports it;
- TTFT;
- decode tokens/s;
- output quality/consistency;
- allocation failures and cache reuse.

Do not set a huge default context just because the option exists. Context cost must be shown to the user.

---

## 11. HCS Model Vault: external HDD, RAM streaming and compression

### 11.1 Storage guarantees and limits

The external USB HDD is a cold model archive and sequential source. It is not RAM/VRAM. It may store many models cheaply and stream their bytes into RAM. Repeated random HDD reads during token generation or every diffusion step can be extremely slow.

HCSInfer may promise:

- no HCS-owned decompressed model cache on SSD by default;
- model source files may remain on the external HDD;
- bounded RAM buffers and model-prefetch pools;
- clear cold/warm load measurement;
- user-configurable model storage root.

HCSInfer must **not** promise:

- Windows will never page memory to its configured system page file;
- HDD-backed weights are as fast as resident RAM;
- lossless compression always shrinks quantized weights;
- streaming from a slow HDD improves generation throughput when the active working set does not fit.

Do not disable the Windows page file to make a marketing claim.

### 11.2 Stage A — use upstream capabilities first

For llama.cpp, benchmark current supported load modes documented by the pinned version, including `mmap`, `mlock`, `mmap+mlock`, Direct-I/O if available and lazy/on-demand tensor loading where supported. The server documentation currently describes `--load-mode` and `--lazy-mode`; names and availability must be confirmed with the pinned binary's `--help`.

For stable-diffusion.cpp, test:

- compute backend separately from parameter backend;
- CPU parameter placement;
- disk parameter placement only as a capacity fallback;
- module-specific placement;
- auto-fit/max-memory budget;
- async next-segment prefetch and diagnostic disable switch, where supported by pinned revision.

Do not automatically choose disk parameters. The upstream performance guide says disk parameters can lower source-memory residency but can be slower because evicted weights are read again.

### 11.3 Stage B — bounded asynchronous pipeline

```text
USB HDD
  -> asynchronous sequential reads
  -> 2–3 bounded compressed buffers (RAM)
  -> CPU decompression workers
  -> integrity verification
  -> verified RAM pages / model-region abstraction
  -> GGML host buffers and Vulkan buffers
```

Use Windows overlapped file I/O and I/O completion ports where appropriate. Candidate APIs:

- `CreateFileW` with `FILE_FLAG_OVERLAPPED`;
- `CreateIoCompletionPort`;
- `GetQueuedCompletionStatus` / `GetQueuedCompletionStatusEx`;
- `ReadFile` with a valid `OVERLAPPED` structure;
- `FILE_FLAG_SEQUENTIAL_SCAN` as an experimentally benchmarked hint;
- optional `FILE_FLAG_NO_BUFFERING` only as a separate measured mode due to alignment constraints.

Use bounded queues and explicit cancellation. Avoid creating a full compressed copy and a full decompressed copy at the same time unless RAM measurement shows it is safe. Prefer sequential reads and tensor-range-aware read-ahead; avoid many random seeks on a spinning disk.

### 11.4 Stage C — HCSMODEL format

Do not replace GGUF or safetensors as the canonical model format in V1. An optional HCSMODEL wrapper/container may be developed after the original formats work.

Required layout:

```text
HCSMODEL
  Header:
    magic
    format_version
    header_size
    model_id
    architecture
    quantization
    source_manifest_hash
    chunk_table_offset (u64)
    chunk_count (u64)
    endianness marker

  Chunk table:
    region/tensor id
    file offset (u64)
    compressed size (u64)
    raw size (u64)
    codec id (raw / LZ4 / Zstd)
    checksum/hash

  Payload:
    independently decodable chunks
```

Parser security rules:

- validate magic, version, header and table sizes before allocations;
- validate every `offset + size` with checked arithmetic and file bounds;
- reject overlaps unless the format explicitly permits and validates them;
- cap tensor/chunk count and maximum declared allocation;
- reject unknown codecs/flags;
- verify checksum before data is passed to an engine;
- validate endian/architecture compatibility;
- reject unsupported compression settings cleanly;
- fuzz every parser entry point.

Compression rules:

- implement Raw first;
- add LZ4 for speed-oriented experiments and Zstandard for ratio/throughput experiments;
- benchmark each chunk on the actual CPU/HDD;
- preserve raw chunks if savings do not exceed a configurable threshold;
- verify decompression is byte-exact;
- never use lossy compression in the Model Vault path;
- keep quantization (a model representation choice) separate from lossless storage compression.

Start by testing 4, 8, 16, 32 and 64 MiB chunk candidates. These are experimental values, not hard-coded optima.

### 11.5 Storage benchmarks

Compare at least:

- raw GGUF from HDD;
- existing mmap mode;
- existing load mode alternatives;
- raw chunked reads;
- LZ4 chunked reads;
- Zstandard low-level chunked reads;
- RAM-prefetched model;
- stable-diffusion.cpp CPU params vs disk params.

Record:

- cold and warm model-ready time;
- time to first token/image;
- sequential read MB/s;
- random I/O count/latency where measurable;
- decompression MB/s;
- CPU time/usage during decompression;
- peak RAM/commit;
- actual compressed size ratio;
- total end-to-end time.

Cold and warm means must be reported separately. Windows file caching can make a second run appear much faster; do not call it a storage algorithm improvement without controlled measurements.

---

## 12. Vulkan shader and kernel engineering

### 12.1 First profile, then write kernels

Use the current GGML Vulkan backend as baseline. Collect operator-level timing and memory/dispatch information first. Only implement an HCS kernel for a demonstrated hotspot.

### 12.2 Hardware and feature query

At runtime query:

- vendor/device ID and device name;
- driver and Vulkan API version;
- subgroup size and supported subgroup operations;
- storage buffer and descriptor limits;
- max workgroup size/invocations;
- shared/local data store limits where exposed through relevant properties;
- shader float16/int8 capabilities where exposed;
- required extensions/features;
- memory heaps/types and budget extension where available;
- queue families and compute queue capabilities.

Do not assume `subgroupSize == 32`, wave32, FP16 performance, cooperative matrices, integer dot product or any optional extension from the Radeon 680M name. Select an implementation only when the runtime query and pipeline creation prove support.

### 12.3 Shader build pipeline

- Author compute shaders in GLSL where appropriate.
- Compile during build with the SDK compiler (`glslc`) to SPIR-V.
- Validate each module with `spirv-val` against the declared target environment.
- Optionally disassemble with `spirv-dis` for debugging.
- Embed SPIR-V in the application or package it in a controlled resource bundle during development.
- The end-user must not need the Vulkan SDK or GLSL compiler.
- Never load arbitrary user-provided shader binaries in V1.
- Record shader source hash, compiler version, target Vulkan/SPIR-V version and compile flags.

Example pattern (adjust the target environment to the minimum feature level the code actually uses):

```powershell
glslc --target-env=vulkan1.2 -fshader-stage=compute `
  shaders/llm/example.comp -o build/shaders/example.spv
spirv-val --target-env vulkan1.2 build/shaders/example.spv
```

Do not target Vulkan 1.3 merely because the SDK supports it if the end-user driver only supports a lower version. Keep the minimum runtime requirement explicit.

### 12.4 Kernel implementation rules

- Keep CPU reference code for each experimental kernel.
- Keep upstream Vulkan fallback.
- Test tensor shapes, strides, layouts, offsets and alignment.
- Use checked index calculations and guard tails.
- Avoid assuming contiguous tensors unless the adapter proves it.
- Treat NaN/Inf and zero/one-element dimensions deliberately.
- Avoid giant temporary FP16 buffers when fusion or blockwise processing is mathematically and numerically valid.
- Consider dispatch overhead and occupancy; tiny operations may be faster on CPU.
- Benchmark warm pipeline execution separately from shader compilation/pipeline creation.
- Include register pressure and workgroup/local-memory constraints in tuning.
- Use timeline/semaphore/barrier correctness; no data race or unsynchronised cross-queue usage.
- Destroy Vulkan resources in dependency-safe order.
- On `VK_ERROR_DEVICE_LOST`, stop submitting work, mark context lost, release safe host resources and rebuild device state only through a controlled recovery path.

### 12.5 Kernel registry and autotuner

Cache a kernel profile keyed by at least:

- GPU vendor/device ID;
- driver version;
- Vulkan API version;
- shader source/binary hash;
- kernel version;
- tensor shape bucket;
- datatype and quantization format;
- relevant operation flags.

Autotuning requirements:

- candidate configurations are bounded and safe;
- never write persistent profiles until correctness passes;
- warm up each candidate;
- perform multiple timed repetitions;
- reject timeouts, validation errors and numerical failures;
- store median and variance, not a single fastest outlier;
- keep a known-good fallback;
- allow reset/rebenchmark;
- never change clocks, BIOS settings or driver policy.

### 12.6 Initial kernel priorities

LLM:

1. quantized GEMV (decode);
2. quantized GEMM (prefill);
3. fused dequantization + matmul;
4. RMSNorm / residual fusion;
5. RoPE;
6. SwiGLU and elementwise fusions;
7. attention and KV read/write;
8. optional quantized KV operations.

Diffusion:

1. measured convolution/GEMM hotspots;
2. attention;
3. normalization/activation fusion;
4. VAE encode/decode and tiled fallback;
5. latent update/scheduler operations.

Do not implement all operators in a single PR. One kernel = one isolated feature, tests, benchmark and rollback path.

---

## 13. CPU/GPU scheduling

### 13.1 Initial profiles

Start deterministic and explainable:

- `CPU`: CPU backend only;
- `Vulkan`: Vulkan where supported, CPU fallback for unsupported operations;
- `Auto`: select the known-good upstream configuration based on current device and memory budget;
- `Balanced`: preserve OS memory headroom and unload inactive contexts;
- `LowMemory`: lower context/batch/resolution, conservative prefetch, CPU or disk placement only if needed.

### 13.2 Per-operation scheduling (later)

A task descriptor may include:

- operator type;
- tensor shape/strides;
- datatype/quantization;
- estimated temporary memory;
- measured CPU time history;
- measured Vulkan time history;
- memory pressure;
- backend support state;
- transfer/dispatch/synchronisation overhead.

Use measured moving averages with a transparent threshold. Do not add a machine-learning scheduler or reinforcement learning to V1 unless a strong measured case exists.

### 13.3 Scheduling safety

- Do not split every small op between CPU and GPU.
- Do not assume UMA removes all transfer/synchronisation costs.
- Account for CPU and GPU sharing memory bandwidth.
- If a Vulkan kernel fails, fall back only when the operation is safe to repeat and its inputs are still valid.
- Never replay a partially committed stateful operation without a transaction/recovery design.
- Record why the scheduler chose a backend in diagnostic mode.

---

## 14. GUI, CLI, API and MCP contracts

### 14.1 GUI

Recommended V1 sections:

- Chat;
- Vision;
- Image Studio;
- Models;
- Performance;
- Settings.

A desktop UI must be fully functional. Avoid inert buttons, fake progress, mock-only content and controls with no implementation. The UI must report:

- selected model and supported modes;
- current backend (CPU/Vulkan) and device;
- loading/generation progress where real progress exists;
- current memory and estimated context budget;
- unsupported feature/model errors;
- cancellation and failure reason.

Simple Mode: acceleration (`Auto`, `Vulkan + CPU fallback`, `CPU only`), memory (`Auto`, `Balanced`, `Low memory`), external model folder, context preset and image preset.

Advanced Mode: thread count, GPU layers, cache type, batch, Vulkan device, tile sizes, prefetch, compression mode, per-kernel override. Explain every option and provide reset.

Keep GUI rendering independent of inference-device availability. If Vulkan initialization fails, the UI still starts and CPU-only mode remains usable.

### 14.2 CLI public contract

The following are desired command names; implement them with real behavior and stable help output:

```powershell
HCSInfer.exe doctor
HCSInfer.exe doctor --json
HCSInfer.exe models scan E:\HCS\Models
HCSInfer.exe models list
HCSInfer.exe models inspect <model-id>
HCSInfer.exe models load <model-id>
HCSInfer.exe models unload
HCSInfer.exe chat --model <model-id>
HCSInfer.exe vision --model <model-id> --image .\image.png --prompt "Describe this image"
HCSInfer.exe image generate --model <model-id> --prompt "..."
HCSInfer.exe image edit --model <model-id> --input .\input.png --prompt "..."
HCSInfer.exe serve --host 127.0.0.1 --port 8765
HCSInfer.exe mcp stdio
HCSInfer.exe benchmark --suite quick
HCSInfer.exe benchmark --suite full --export .\benchmark.json
HCSInfer.exe tune --device radeon-680m
HCSInfer.exe cache stats
HCSInfer.exe cache clear
```

CLI rules:

- `--help` works at root and every subcommand;
- stable exit codes for invalid args, model errors, backend errors, OOM, cancellation and internal errors;
- JSON output for automation;
- no destructive default behavior;
- paths are validated against approved roots;
- commands copied into README must be covered by tests.

### 14.3 HTTP API

OpenAI-compatible routes only where request/response semantics genuinely match:

```text
GET  /v1/models
POST /v1/chat/completions
POST /v1/images/generations
POST /v1/images/edits
```

HCS-native routes:

```text
GET  /hcs/v1/health
GET  /hcs/v1/runtime
GET  /hcs/v1/devices
GET  /hcs/v1/memory
GET  /hcs/v1/models
POST /hcs/v1/models/{id}/load
POST /hcs/v1/models/{id}/unload
POST /hcs/v1/benchmarks
GET  /hcs/v1/jobs/{id}
POST /hcs/v1/jobs/{id}/cancel
```

API requirements:

- bind to `127.0.0.1` by default;
- random local token or an equally strong local access policy;
- explicit LAN opt-in with authentication;
- strict body-size and image-pixel limits;
- timeouts, rate/concurrency bounds and cancellation;
- request IDs and structured errors;
- streaming tests;
- no prompt/image content in logs by default;
- validate model IDs and filesystem paths;
- no arbitrary path read, shell, process execution or unbounded memory allocation.

Long-running tasks should return a job ID or a stream with progress, and support cancellation.

### 14.4 MCP

Implement stdio first. Add Streamable HTTP only after conformance/security tests.

Desired tools:

- `hcs.models.list`;
- `hcs.models.inspect`;
- `hcs.models.load`;
- `hcs.models.unload`;
- `hcs.chat.generate`;
- `hcs.vision.analyze`;
- `hcs.image.generate`;
- `hcs.image.edit`;
- `hcs.runtime.status`;
- `hcs.runtime.benchmark`;
- `hcs.jobs.status`;
- `hcs.jobs.cancel`.

MCP rules:

- narrow typed JSON Schemas;
- no arbitrary shell execution;
- no unrestricted filesystem access;
- use approved model/input/output roots;
- bound image dimensions, content size, tool duration and concurrency;
- propagate cancellation;
- tool errors must be actionable and not leak secrets;
- use the exact protocol version supported by the implementation and tested clients;
- stdio logs go to stderr, not stdout, so they cannot corrupt the JSON-RPC stream.

The MCP `2026-07-28` Streamable HTTP specification changes transport behavior (including removal of the GET stream endpoint and protocol-level sessions). Do not implement it based on an older example. Validate `Origin`, reject invalid origins, bind to loopback by default and authenticate any non-loopback deployment. If the chosen C++ SDK does not support the required version, either implement the required subset correctly or keep Streamable HTTP out of V1; never claim conformance based on a protocol version string alone.

---

## 15. Research-to-code rules (SOTA must become tested implementation)

Research is not implemented by mentioning a paper in README. Each research idea requires a documented hypothesis, exact algorithm scope, hardware assumptions, baseline, code, correctness and performance proof.

### 15.1 FlashAttention / IO-aware attention

Paper: Dao et al., *FlashAttention: Fast and Memory-Efficient Exact Attention with IO-Awareness*, NeurIPS 2022.

https://proceedings.neurips.cc/paper_files/paper/2022/hash/67d57c32e20fd0a7a302cb81d36e40d5-Abstract-Conference.html

HCSInfer implementation direction:

- adapt the tiled/IO-aware principle, not CUDA-specific implementation assumptions;
- respect Radeon 680M local/shared-memory, subgroup and register constraints;
- avoid materializing a full large attention matrix where a mathematically equivalent tiled implementation is viable;
- test attention output against a high-precision/reference implementation;
- compare against upstream GGML Vulkan attention for every supported shape;
- only activate when the actual model/backend uses the kernel and measured latency/memory improves.

### 15.2 PagedAttention / block KV cache

Paper: Kwon et al., *Efficient Memory Management for Large Language Model Serving with PagedAttention*, 2023.

https://arxiv.org/abs/2309.06180

HCSInfer direction:

- block-based KV storage;
- explicit block table and allocator;
- context-growth and fragmentation tests;
- safe block reclamation and cache reuse;
- single-user memory predictability first.

Do not copy vLLM's multi-user throughput claims onto a single-user mini PC without measurement.

### 15.3 AWQ

Paper: *AWQ: Activation-aware Weight Quantization for On-Device LLM Compression and Acceleration*, MLSys 2024.

https://proceedings.mlsys.org/paper_files/paper/2024/hash/42a452cbafa9dd64e9ba4aa95cc1ef21-Abstract-Conference.html

HCSInfer direction:

- research an optional quantization analysis/export tool;
- ensure the selected format is supported by the model loader and Vulkan/CPU kernels;
- compare perplexity or appropriate evaluation, not just file size;
- do not call a model “AWQ accelerated” if only metadata is changed or it is converted to an unsupported format.

### 15.4 SmoothQuant

Paper: *SmoothQuant: Accurate and Efficient Post-Training Quantization for Large Language Models*, ICML 2023.

https://proceedings.mlr.press/v202/xiao23c.html

HCSInfer direction:

- study activation/weight scaling and W8A8 feasibility;
- implement only if target backend has efficient integer arithmetic for the required operator;
- measure quality, memory and latency;
- otherwise retain as documented research, not a shipped feature.

### 15.5 Speculative decoding

Start with the pinned llama.cpp upstream implementation (draft model and/or supported n-gram methods). Measure draft overhead, acceptance rate, memory cost and tokens/s. On an 8-core APU a second resident model may cost more than it saves. Prefer the smallest feasible experiment and keep it optional.

### 15.6 Heterogeneous scheduling

Use measured operator cost and memory-pressure data. Start with transparent deterministic heuristics. Keep scheduler decisions inspectable. Do not use reinforcement learning or an opaque adaptive controller for V1.

### 15.7 Research note template

Every feature under `docs/research/<topic>.md` must contain:

```text
Title and status
Paper / DOI / official repository
Date and commit/version examined
Problem and hypothesis
Mathematical/algorithm summary
Hardware and feature requirements
Upstream implementation comparison
Expected benefit and possible regressions
Implementation plan and feature flag
Test vectors and numerical tolerance
Benchmark method and baseline
Measured results with machine metadata
Decision: accept / reject / defer
Limitations and fallback
```

No invented SOTA results. Distinguish paper-published results from HCSInfer's measured results.

---

## 16. Testing strategy — mandatory gates

### 16.1 Test layers

**A. Static/build checks**

- CMake configure for supported presets;
- Release and Debug build;
- compiler warnings on HCS code;
- format check;
- static analysis where supported;
- dependency/licence/SBOM audit;
- SPIR-V compile/validation;
- `ctest -N` verifies expected test discovery.

**B. Unit tests**

- model registry and metadata;
- config parsing/migration;
- memory budget arithmetic;
- allocation lifecycle;
- job queue/cancellation;
- scheduler backend decision;
- chunk table and bounds checks;
- compression/decompression round-trip;
- hash/checksum verification;
- API schemas and error mapping;
- MCP JSON-RPC and tool schemas;
- path canonicalisation and approved-root checks.

**C. Adversarial parser tests**

- truncated header/table/payload;
- malformed metadata;
- invalid dimensions;
- integer overflow in `offset + size`;
- overlapping chunks;
- invalid codec IDs;
- wrong hashes;
- excessive tensor/chunk counts;
- huge declared allocations;
- corrupt compressed blocks;
- path traversal and symlink escape;
- interrupted HDD reads and unplug/reconnect.

**D. Kernel correctness tests**

- CPU reference vs upstream Vulkan vs HCS kernel;
- tiny, odd, non-power-of-two and large shapes;
- every supported dtype/quant block boundary;
- alignment, offsets, strides and non-contiguous layouts;
- tail handling and bounds checks;
- NaN/Inf and zero/one edge cases as appropriate;
- absolute + relative numeric tolerances;
- validation layers and device-loss behaviour.

**E. Real-model integration tests**

- actual small GGUF text model from HDD;
- actual supported VLM and projector with a fixed test image;
- actual small diffusion model with low-resolution output;
- API calls for real inference;
- MCP client invoking real tools;
- GUI workflow without mocks;
- CPU-only fallback;
- model switch between VLM / diffusion / text;
- cancellation and shutdown.

**F. Performance tests**

Measure cold and warm independently:

- process start to runtime ready;
- model discovery and validation time;
- model load time;
- TTFT;
- prompt processing tokens/s;
- generation tokens/s;
- image generation wall time and steps/s;
- peak RAM/commit;
- Vulkan memory budget/usage;
- HDD read MB/s and I/O count/latency;
- decompression MB/s and CPU use;
- GPU utilisation/clock/temperature if measurable;
- time to unload and memory returned;
- failure rate and device-lost events.

Use fixed model hashes, prompts, context, seed, output dimensions, same driver/build, warm-up, thermal settling, multiple repetitions, median and p90. Keep raw JSON results.

**G. Soak/reliability tests**

- repeated load/unload cycles (at least 100 for nightly where practical);
- 30-minute text-generation run;
- repeated diffusion smoke generation;
- repeated VLM image analysis;
- 6–12-hour soak on scheduled/self-hosted runner where hardware permits;
- USB disconnect during loading;
- OOM injection/simulation at allocator boundary;
- Vulkan device-loss simulation where possible;
- API client disconnect/cancellation;
- app shutdown while idle, loading and generating.

### 16.2 No mocks for release acceptance

Mocks are allowed for isolated GUI/API state tests and fault injection. Mocks are never accepted as evidence for inference, model loading, Vulkan execution, image generation or MCP end-to-end functionality. Release evidence must include real models and actual backend identity. If physical GPU is unavailable in CI, label GPU test `NOT RUN` or `BLOCKED` and run it on the self-hosted target machine before release.

### 16.3 GoogleTest and CTest

Use a pinned GoogleTest dependency (submodule or pinned FetchContent commit) and CTest discovery. See official quickstart:

https://google.github.io/googletest/quickstart-cmake.html

Typical CMake setup:

```cmake
enable_testing()
include(GoogleTest)
gtest_discover_tests(hcsinfer_unit_tests)
```

Typical commands:

```powershell
ctest --test-dir build/windows-release -C Release --output-on-failure
ctest --test-dir build/windows-release -C Release -L unit --output-on-failure
ctest --test-dir build/windows-release -C Release -R "ModelVault|MemoryBudget" --output-on-failure
```

Use labels for `unit`, `contract`, `parser`, `kernel-cpu`, `kernel-vulkan`, `integration`, `e2e`, `performance`, `soak`.

### 16.4 Sanitizers and fuzzing

- Clang libFuzzer is preferred for parser fuzz targets where supported.
- Combine with AddressSanitizer and UndefinedBehaviorSanitizer on supported Clang builds.
- MSVC AddressSanitizer may be used for Windows-specific code where supported.
- Keep fuzz targets deterministic and bounded for CI smoke runs; use longer scheduled fuzz runs.
- Seed corpora with valid and malformed synthetic GGUF/HCSMODEL headers and compressed chunks; do not commit copyrighted model weights.

libFuzzer documentation: https://llvm.org/docs/LibFuzzer.html

Example Clang fuzz build shape (adapt to actual CMake target):

```bash
clang++ -g -O1 -fsanitize=fuzzer,address,undefined \
  tests/parser-fuzz/hcsmodel_fuzz.cpp src/storage/hcsmodel_parser.cpp \
  -o build/hcsmodel_fuzz
./build/hcsmodel_fuzz -max_total_time=60 tests/corpus/hcsmodel
```

Do not claim UBSan is active in a compiler configuration that does not support it.

### 16.5 SPIR-V checks

Every generated `.spv` module must pass `spirv-val` for the declared target environment. Add a CI check that compiles all shader sources and validates each output. Khronos SPIR-V Tools: https://github.com/KhronosGroup/SPIRV-Tools

### 16.6 Benchmark matrix

Maintain stable benchmark IDs:

| ID | Backend/storage | Purpose |
|---|---|---|
| B0 | CPU, raw GGUF from HDD | CPU baseline |
| B1 | Vulkan, raw GGUF from HDD | upstream Vulkan baseline |
| B2 | upstream best CPU/Vulkan placement | reference best configuration |
| B3 | HCS kernel + fallback | isolated kernel impact |
| B4 | raw chunked HDD reads | Model Vault baseline |
| B5 | LZ4 chunk loading | compression trade-off |
| B6 | Zstd low-level chunk loading | compression trade-off |
| B7 | sdcpp CPU params vs disk params | diffusion storage/offload |
| B8 | RAM-prefetched model | isolate storage load from inference |

A performance PR must include baseline and new result on the same environment. Use an agreed regression alert threshold (e.g. >5% median regression after repeated runs) but do not fail on a single noisy run. Correctness, memory safety, crash and security failures are hard failures.

---

## 17. Self-healing implementation playbooks

### 17.1 Build failure loop

1. Save complete configure/build output.
2. Determine whether failure is missing dependency, generator mismatch, CMake option drift, compile error, link conflict or toolchain mismatch.
3. Check upstream docs for the exact pinned commit.
4. Inspect generated `CMakeCache.txt` and target graph.
5. Change one variable at a time.
6. Reconfigure into a fresh build directory if generator/compiler/options changed.
7. Rebuild target and rerun tests.
8. Record final known-good build command in `docs/build.md`.

### 17.2 GGML integration loop

1. Build llama.cpp with its own GGML.
2. Build stable-diffusion.cpp with its patched GGML.
3. Build stable-diffusion.cpp with `SD_USE_UPSTREAM_GGML=ON` and the exact upstream source directory.
4. Run representative diffusion models in both modes.
5. Compare operator availability, output, performance and warnings.
6. Attempt combined CMake graph only after separate builds pass.
7. Detect duplicate target names, duplicate symbols, incompatible private headers and compile-definition mismatches.
8. If incompatibility remains, isolate the adapters and keep a multi-binary dev package. Do not force a common ABI.
9. Document the blocked one-EXE condition and keep other milestones moving.

### 17.3 Vulkan failure loop

1. Run `vulkaninfo --summary`.
2. Record installed AMD/OEM driver and ICD.
3. Run a Vulkan presentation smoke test only as a preliminary check.
4. Run llama.cpp `--list-devices` and a real GPU inference test.
5. Enable Vulkan validation layers for the minimal reproducer.
6. Capture validation output, device limits, shader hash and driver version.
7. Determine whether the failing feature is core, extension or unsupported.
8. Add feature query and fallback if optional.
9. Fix synchronisation/resource lifetime if correctness issue.
10. Rerun CPU fallback, Vulkan test, full kernel suite and real model test.

Do not “fix” driver failures by enabling unsupported device features or suppressing validation errors.

### 17.4 Kernel regression loop

1. Run CPU reference and upstream kernel on identical inputs.
2. Validate input shape/strides and quantization blocks.
3. Run HCS kernel with validation enabled.
4. Compare results with exact/type-specific tolerances.
5. Capture per-stage timing, dispatch count, memory and validation output.
6. Inspect register pressure, occupancy proxy, workgroup size, memory traffic and dispatch overhead.
7. Change one kernel parameter or algorithm at a time.
8. Re-run correctness before timing.
9. Reject candidates that are faster only in a single outlier or fail any tested shape.
10. Keep the best known-good implementation and feature-flag all experiments.

### 17.5 Memory/OOM loop

1. Capture current budget and allocations by owner.
2. Identify model weights, KV cache, workspace, Vulkan buffers, prefetch and UI usage.
3. Reproduce with fixed model/context/image size.
4. Check ownership/lifetime and whether a buffer is double-counted.
5. Add allocation limits and graceful failure before allocation.
6. Check cleanup after success, failure and cancellation.
7. Re-run leak/soak/model-switch tests.
8. Provide user-facing advice (unload model, reduce context/size, use quant, CPU mode) rather than silently shrinking output parameters.

### 17.6 Model Vault/I/O loop

1. Verify source file identity and hash.
2. Measure raw sequential HDD performance.
3. Measure decompression independently from I/O using RAM-resident test data.
4. Measure I/O independently with raw reads.
5. Measure combined pipeline.
6. Vary one chunk/buffer/worker parameter.
7. Keep memory bounded and verify back-pressure.
8. Simulate disconnect/error/cancel.
9. Ensure no HCS-owned SSD staging file was created.
10. Compare cold/warm tests and document Windows file cache effects.

### 17.7 API/MCP loop

1. Test request schema and authentication first.
2. Test success with a real model.
3. Test malformed, oversized, missing, unauthorized and unsupported inputs.
4. Test timeout and cancellation.
5. Verify no prompt/image content is logged by default.
6. Verify paths remain within approved roots.
7. For MCP, test initialization, protocol version negotiation, tool listing, schema validation, tool call, error response and cancellation.
8. For HTTP transport, test invalid Origin rejection and localhost-only bind.
9. Test stdout cleanliness for stdio transport.

### 17.8 End-of-loop rule

At the end of every milestone:

- update `STATUS.md`;
- record accepted and deferred tasks;
- attach evidence;
- update README if visible capabilities changed;
- run milestone regression suite;
- commit only when tree is clean enough and all relevant tests pass;
- continue to the next unblocked milestone.

---

## 18. GitHub Actions and self-hosted Radeon runner

### 18.1 CI split

Use GitHub-hosted runners for portable checks and a self-hosted Windows 11 machine for real Radeon 680M/Vulkan validation.

GitHub-hosted jobs:

- Windows configure/build;
- Linux configure/build;
- unit/contract tests;
- clang-format and clang-tidy where feasible;
- parser fuzz smoke;
- shader compile + `spirv-val`;
- API/MCP contract tests;
- dependency/licence/SBOM checks;
- package reproducibility checks.

Self-hosted target runner:

- real Vulkan device and feature report;
- Radeon 680M kernel tests;
- real text model inference;
- real VLM image analysis;
- real diffusion smoke test;
- cold/warm HDD loading;
- memory switching and soak tests.

### 18.2 Self-hosted runner security

- Use a dedicated local Windows account and dedicated machine profile.
- Do not store secrets with broad access on a self-hosted runner.
- Do not execute untrusted pull-request code on a persistent runner with personal files/credentials.
- Prefer manual or trusted-branch dispatch for hardware tests.
- Run in a restricted workspace and clean only its own artifact directory.
- Keep model weights outside Git and ensure test models have clear licences.
- Record driver, system and model hashes on every run.

### 18.3 Workflow gates

PR quick gate:

1. format/static checks;
2. configure and compile HCS targets;
3. unit and contract tests;
4. parser fuzz smoke;
5. shader compile/validate;
6. API/MCP contract tests;
7. no licence/security critical failure.

Nightly gate:

1. full unit/integration suite;
2. real hardware/model tests on trusted self-hosted runner;
3. benchmark collection;
4. soak test;
5. regression report.

Release gate:

1. all required milestones green;
2. clean-room Windows packaging test;
3. real Radeon Vulkan test;
4. CPU fallback test;
5. model switching and long-run tests;
6. security/licence/SBOM audit;
7. checksum/signing where possible;
8. complete release notes and support matrix.

---

## 19. Security, privacy, reliability and hardening

### 19.1 Network security

- Default bind `127.0.0.1` only.
- LAN bind is explicit and requires a generated token or robust configured authentication.
- Validate `Origin` for browser-facing and MCP HTTP requests.
- Apply strict CORS allowlist; never wildcard sensitive local API routes.
- Rate-limit and bound concurrent inference jobs.
- Enforce request/body/image size caps and timeouts.
- Protect against DNS rebinding on local HTTP endpoints.
- Do not expose model-control or filesystem tools to an unauthenticated network.

### 19.2 MCP tool security

- No arbitrary shell, process-spawn or unrestricted filesystem tool.
- Only allow model roots and input/output roots explicitly selected by the user.
- Canonicalise paths and guard against `..`, symlinks/junctions and reparse-point escape.
- Use typed input schemas and enum constraints.
- Validate all model IDs and job IDs.
- Use per-tool memory/time/concurrency limits.
- Cancel jobs and release resources on disconnect when configured.

### 19.3 Untrusted model and image files

Treat GGUF, safetensors, HCSMODEL, compressed chunks, images and metadata as untrusted input.

- Validate all sizes and offsets before allocation/read.
- Guard integer overflow.
- Cap image decoded pixels and dimensions.
- Avoid unsafe deserialisation of arbitrary object formats.
- Fuzz parsers and image decoders.
- Verify hashes where manifests provide them.
- Do not trust file extension alone.
- Never execute content from model files.

### 19.4 Privacy

- Local-first; no network request except explicit model downloads or user-enabled features.
- No prompt/image content in telemetry by default.
- Diagnostic bundle requires explicit user action and offers preview/redaction.
- API tokens and credentials live in OS-protected local storage or an appropriately secured local config, never Git.
- Model URLs and file paths may reveal user information; redact in public issue reports.

### 19.5 Reliability

- RAII and deterministic shutdown.
- cancellation-safe jobs;
- bounded queues and backpressure;
- file handle and USB device loss detection;
- Vulkan device-lost recovery path;
- OOM graceful handling;
- atomic config writes and schema migration;
- crash log without secrets/prompt content;
- corrupted cache invalidation and rebuild;
- no state mutation from background callbacks after owner destruction.

### 19.6 Dependency and binary hardening

- pin dependencies;
- check licences;
- maintain SBOM;
- scan known vulnerabilities using available tooling;
- compile release without debug secrets/symbol paths where feasible;
- sign binaries when certificate access is available;
- publish SHA-256 checksums;
- verify downloaded tool archives using publisher hashes/signatures;
- do not download executable dependencies from arbitrary forum links.

---

## 20. Compatibility, extensibility and long-term maintainability

The architecture must be extendable without tying the whole application to one GPU, model or OS.

- Hardware detection is capability-based, not name-based.
- CPU executor is independent of Vulkan executor.
- Vulkan device/backend selection supports more than one device in principle, even if V1 tests only one iGPU.
- Engine adapters isolate upstream version changes.
- Model manifest contains architecture and capability metadata.
- API versions are explicitly versioned.
- HCSMODEL has a versioned header and migration policy.
- Config files have schema versions and forward/backward compatibility rules.
- Unknown config keys should be handled safely and documented.
- New model types are added through adapter/capability registrations, not ad-hoc UI branching.
- Kernel registry has safe fallback and versioned cache keys.
- Avoid global state and hidden ownership.
- Avoid unnecessary template complexity or premature plugin systems.
- Use feature flags for experimental kernels and research paths.

Do not claim general RDNA compatibility after testing only Radeon 680M. Publish a tested hardware matrix: exact GPU/device ID, driver, OS, Vulkan version, supported features, test coverage and status.

---

## 21. Performance proof and release benchmarks

### 21.1 Required benchmark metadata

Every benchmark JSON must include:

```json
{
  "schema_version": 1,
  "timestamp_utc": "...",
  "git_commit": "...",
  "build_type": "Release",
  "compiler": "...",
  "cmake_version": "...",
  "os_build": "...",
  "cpu": "...",
  "gpu": "...",
  "gpu_vendor_id": "...",
  "gpu_device_id": "...",
  "driver_version": "...",
  "vulkan_api_version": "...",
  "ram_bytes": 0,
  "model_sha256": "...",
  "model_format": "GGUF",
  "quantization": "...",
  "context_tokens": 0,
  "backend": "CPU|Vulkan|Hybrid",
  "storage_source": "USB-HDD",
  "cold_or_warm": "cold",
  "warmup_iterations": 0,
  "measured_iterations": 0,
  "ttft_ms_median": 0,
  "prompt_tokens_per_second_median": 0,
  "decode_tokens_per_second_median": 0,
  "peak_ram_bytes": 0,
  "hdd_read_mib_s_median": 0,
  "decompression_mib_s_median": 0,
  "result_status": "PASS"
}
```

The schema may evolve; record its version and validate output.

### 21.2 Benchmark integrity

- Do not compare different model hashes or quantizations as a backend-only comparison.
- Do not compare different prompts/context/output sizes.
- Do not compare cold vs warm runs.
- Stabilise thermals and record if the system throttles.
- Use multiple samples, median and p90; keep the raw samples.
- Record power plan and background activity when material.
- Do not use one best-of-N sample as “average performance”.
- Every optimization claim links to benchmark artefacts and commit.
- If a result cannot be reproduced, mark it as provisional.

### 21.3 Release claims

Use precise statements such as “median decode throughput improved by X% on Radeon 680M with model hash Y, Q4_K_M, context Z, driver A, build B.” Never say “2x faster on all AMD GPUs” based on one device or synthetic microbenchmark.

---

## 22. V1 definition of done (hard release checklist)

V1 is ready only when each applicable item is proven:

### Build and distribution

- [ ] clean Windows x64 Release configure and build;
- [ ] Debug build and tests;
- [ ] reproducible dependency pins and build command;
- [ ] no unexplained compiler/linker warnings in HCS code;
- [ ] shader compilation and SPIR-V validation pass;
- [ ] one-EXE requirement passes clean-machine test, or release explicitly remains a portable multi-file package and is not labelled single-file;
- [ ] no Vulkan SDK required on the end-user machine;
- [ ] external models and system AMD/Vulkan driver documented;
- [ ] SHA-256 checksum and SBOM generated;
- [ ] license and third-party notices complete.

### Functional product

- [ ] actual GGUF text generation;
- [ ] actual supported VLM image analysis;
- [ ] actual diffusion image generation;
- [ ] model manager scan/inspect/load/unload;
- [ ] model switch and memory reclamation;
- [ ] GUI normal-user workflow;
- [ ] CLI workflows;
- [ ] API real-inference tests;
- [ ] MCP stdio real-client tests;
- [ ] CPU-only fallback;
- [ ] clear unsupported model/backend errors.

### Storage and memory

- [ ] external USB HDD model root;
- [ ] bounded asynchronous loading/prefetch;
- [ ] no HCS-owned decompressed SSD model cache by default;
- [ ] Windows page-file limitation documented;
- [ ] corrupt/truncated model tests;
- [ ] USB disconnect recovery;
- [ ] memory budget and OOM handling;
- [ ] cold/warm load benchmarks.

### Compute correctness and performance

- [ ] upstream CPU baseline;
- [ ] upstream Vulkan baseline on real Radeon 680M;
- [ ] real HCS kernel tests for each kernel enabled by default;
- [ ] CPU/reference numeric correctness;
- [ ] no default HCS kernel slower than selected fallback for its enabled shape/device profile unless consciously accepted and documented;
- [ ] scheduler decision and fallback are observable;
- [ ] performance results are reproducible and metadata-complete.

### Security and reliability

- [ ] API localhost default and authentication policy;
- [ ] MCP no arbitrary shell/filesystem access;
- [ ] Origin validation for HTTP MCP;
- [ ] path traversal/symlink tests;
- [ ] parser fuzz smoke and scheduled fuzzing;
- [ ] image size limits;
- [ ] cancellation and shutdown tests;
- [ ] model switch/soak tests;
- [ ] no known critical vulnerability in bundled dependencies;
- [ ] privacy/logging policy documented.

### Documentation

- [ ] README feature matrix matches the binary;
- [ ] quick start tested from a clean machine;
- [ ] supported models table;
- [ ] troubleshooting;
- [ ] API and MCP docs;
- [ ] Model Vault docs;
- [ ] hardware-support matrix;
- [ ] benchmark methodology/results;
- [ ] known limitations and non-goals;
- [ ] release notes.

Any unchecked item is either a release blocker or explicitly excluded from V1 scope in the release notes. Do not silently mark it complete.

---

## 23. Required agent deliverables at every milestone

At the end of each milestone, update:

1. `docs/roadmap/STATUS.md` — concise status, PASS/FAIL/BLOCKED/NOT RUN.
2. `docs/test-evidence/<milestone>/` — command logs, test summaries and JSON results, with secrets and personal paths redacted.
3. `docs/decisions/` — ADR for consequential choices.
4. `README.md` — only if public behaviour/requirements changed.
5. `docs/architecture.md` / relevant subsystem doc.
6. `external/dependency-lock.json` — if any dependency changed.
7. `CHANGELOG.md` or release notes when a user-visible change is complete.

Final report format:

```text
HCSInfer engineering report
- Current phase:
- Commit:
- Build status:
- Tests: PASS / FAIL / BLOCKED / NOT RUN
- Real Radeon 680M test: PASS / FAIL / BLOCKED / NOT RUN
- Real text model: PASS / FAIL / BLOCKED / NOT RUN
- Real VLM: PASS / FAIL / BLOCKED / NOT RUN
- Real diffusion model: PASS / FAIL / BLOCKED / NOT RUN
- API/MCP: PASS / FAIL / BLOCKED / NOT RUN
- Storage/HDD: PASS / FAIL / BLOCKED / NOT RUN
- Security/licence: PASS / FAIL / BLOCKED / NOT RUN
- Performance evidence:
- Known limitations:
- Remaining blockers:
- Exact next action:
```

Do not finish the task by merely repeating the plan. Implement what is possible in the current repository, run it, and report the evidence.

---

## 24. Primary sources, build downloads and technical references

Use the official upstream documentation for the pinned revision. Pages under `master` may change; when using one, record the actual commit and preferably link to a commit-pinned URL in generated project documentation.

### Hardware / driver / Vulkan

- AMD Ryzen 7 7735HS product specifications: https://www.amd.com/en/products/processors/laptop/ryzen/7000-series/amd-ryzen-7-7735hs.html
- AMD 7735HS driver/support page: https://www.amd.com/en/support/downloads/drivers.html/processors/ryzen/ryzen-7000-series/amd-ryzen-7-7735hs.html
- LunarG Vulkan SDK downloads (Windows/Linux, current versions and hashes): https://vulkan.lunarg.com/sdk/home
- Vulkan specification / registry: https://registry.khronos.org/vulkan/
- Vulkan Guide: https://docs.vulkan.org/guide/latest/
- Vulkan ML inference tutorial: https://docs.vulkan.org/tutorial/latest/ML_Inference/introduction.html
- Vulkan validation layers: https://github.com/KhronosGroup/Vulkan-ValidationLayers
- SPIR-V Tools / `spirv-val`: https://github.com/KhronosGroup/SPIRV-Tools
- Mesa RADV: https://docs.mesa3d.org/drivers/radv.html
- Mesa source: https://gitlab.freedesktop.org/mesa/mesa

### Engines / model formats

- llama.cpp repository: https://github.com/ggml-org/llama.cpp
- llama.cpp build guide: https://github.com/ggml-org/llama.cpp/blob/master/docs/build.md
- llama.cpp server documentation: https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md
- llama.cpp multimodal developer guide: https://github.com/ggml-org/llama.cpp/blob/master/tools/mtmd/README-dev.md
- llama.cpp GGML Vulkan backend: https://github.com/ggml-org/llama.cpp/tree/master/ggml/src/ggml-vulkan
- stable-diffusion.cpp repository: https://github.com/leejet/stable-diffusion.cpp
- stable-diffusion.cpp build guide: https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/build.md
- stable-diffusion.cpp backend assignment: https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/backend.md
- stable-diffusion.cpp performance / parameter placement: https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/performance.md
- stable-diffusion.cpp server API: https://github.com/leejet/stable-diffusion.cpp/blob/master/examples/server/api.md
- GGUF specification/tools: https://github.com/ggml-org/ggml/blob/master/docs/gguf.md
- Hugging Face model hub (verify each model card, licence and format): https://huggingface.co/models
- Hugging Face GGUF model filter: https://huggingface.co/models?library=gguf
- Hugging Face text-to-image model filter: https://huggingface.co/models?pipeline_tag=text-to-image

### Build systems / quality

- Visual Studio downloads: https://visualstudio.microsoft.com/downloads/
- MSVC C++ installation: https://learn.microsoft.com/en-us/cpp/build/vscpp-step-0-installation
- CMake downloads: https://cmake.org/download/
- CMake presets: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
- CMake `CMAKE_MSVC_RUNTIME_LIBRARY`: https://cmake.org/cmake/help/latest/prop_tgt/MSVC_RUNTIME_LIBRARY.html
- Ninja releases: https://github.com/ninja-build/ninja/releases
- GoogleTest CMake quickstart: https://google.github.io/googletest/quickstart-cmake.html
- CTest manual: https://cmake.org/cmake/help/latest/manual/ctest.1.html
- clang-tidy: https://clang.llvm.org/extra/clang-tidy/
- clang-format: https://clang.llvm.org/docs/ClangFormat.html
- LLVM libFuzzer: https://llvm.org/docs/LibFuzzer.html
- AddressSanitizer: https://clang.llvm.org/docs/AddressSanitizer.html
- UndefinedBehaviorSanitizer: https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
- GitHub Actions docs: https://docs.github.com/en/actions
- Self-hosted runner security: https://docs.github.com/en/actions/security-for-github-actions/security-guides/security-hardening-for-github-actions

### Storage / compression / Windows I/O

- LZ4 source/docs: https://github.com/lz4/lz4
- Zstandard source/docs: https://github.com/facebook/zstd
- Windows file I/O overview: https://learn.microsoft.com/en-us/windows/win32/fileio/file-management
- Windows overlapped I/O: https://learn.microsoft.com/en-us/windows/win32/fileio/overlapped-i-o
- I/O completion ports: https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports
- CreateFile flags: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
- Windows file-backed/pagefile-backed sections: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/file-backed-and-page-file-backed-sections

### Protocol / API

- Model Context Protocol specification (2026-07-28): https://modelcontextprotocol.io/specification/2026-07-28
- MCP Streamable HTTP transport: https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http
- MCP tools: https://modelcontextprotocol.io/specification/2026-07-28/server/tools
- MCP security best practices: https://modelcontextprotocol.io/specification/2026-07-28/basic/security_best_practices
- MCP official repository: https://github.com/modelcontextprotocol/modelcontextprotocol

### Research

- FlashAttention, NeurIPS 2022: https://proceedings.neurips.cc/paper_files/paper/2022/hash/67d57c32e20fd0a7a302cb81d36e40d5-Abstract-Conference.html
- PagedAttention, 2023: https://arxiv.org/abs/2309.06180
- AWQ, MLSys 2024: https://proceedings.mlsys.org/paper_files/paper/2024/hash/42a452cbafa9dd64e9ba4aa95cc1ef21-Abstract-Conference.html
- SmoothQuant, ICML 2023: https://proceedings.mlr.press/v202/xiao23c.html

---

## 25. Final rule: ship proof, not promises

The first priority is a real, reproducible CPU/Vulkan baseline on the target mini PC. Next unify the engines behind a reliable control plane; then deliver API/MCP and GUI; then implement bounded HDD-to-RAM loading; then prove shared GGML and single-EXE packaging; only then ship custom Vulkan kernels that are demonstrably correct and faster for measured workloads.

HCSInfer V1 is not “done” because all planned files exist or because the compiler returned success. It is done when its real workloads, lifecycle, resource management, security boundaries, storage behaviour, packaging and documentation pass the gates above with traceable evidence.
