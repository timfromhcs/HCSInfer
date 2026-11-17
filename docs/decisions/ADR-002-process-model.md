# ADR-002: One-process target, development process isolation allowed

Status: Accepted (2026-11-17, finalised). Depends on ADR-001 (accepted, scoped).

## Context
Goal is a single `HCSInfer.exe`. `llama.cpp` and `stable-diffusion.cpp` may not share one GGML build
(`stable-diffusion.cpp` defaults to a patched GGML). Linking two GGML copies into one process is unsafe.

## Options
1. Single process, one shared GGML (release goal; only if M7 proves it).
2. Single process, two engine libraries with hidden symbols/separate namespaces.
3. Control plane + one helper process per engine (JSON/pipe IPC), same package.

## Decision
- Control plane, CLI, API, MCP, GUI always live in one process and talk to engines only through
  `ITextEngine` / `IImageEngine` (`include/hcs/engine.hpp`), whose types are opaque value types so the
  boundary is IPC-able.
- Development default: option 1 if proven, else option 3 as a transparent portable multi-file package.
  Option 2 only if symbol isolation is proven. The single-EXE claim stays blocked until M7 evidence exists.
- Never link two incompatible GGML versions into one process.

## Rejected
Forcing a common ABI by patching upstream blindly.

## Risks
IPC adds copy/latency for image bytes and token streaming; mitigated by streaming events already modelled in `JobQueue`.
Lifecycle/cancellation semantics must be identical in both modes.

## Evidence / migration
Option 1 is what the build does today: CLI, API and MCP share one process with both engine adapters and one GGML (ADR-001 evidence). The IPC option (3) was not built and is not needed unless ADR-001's fallback is triggered. The "single `HCSInfer.exe`" claim is not made: that needs static CRT and a clean-machine test (ADR-008). Migration cost is confined to engine adapters.
