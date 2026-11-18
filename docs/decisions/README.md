# Architecture decision records

Each ADR has context, options, decision, rejected alternatives, risks and evidence. Status is copied from the first
lines of the file; read the file for the open items.

| ADR | Title | Status (as written in the file) |
|---|---|---|
| [001](ADR-001-engine-integration.md) | Engine integration and GGML topology | Accepted for V1 development, scoped to SD1.5-class models; broader operator coverage is an open evidence item |
| [002](ADR-002-process-model.md) | One-process target, development process isolation allowed | Accepted (2026-11-17) |
| [003](ADR-003-vulkan-ownership.md) | Vulkan device/queue ownership | Accepted for V1; kernel-related parts (M8) remain a forward decision |
| [004](ADR-004-memory-budget-uma.md) | Memory budget and UMA accounting | Accepted with open validation |
| [005](ADR-005-model-vault-no-ssd-cache.md) | Model Vault and the no-SSD-app-cache policy | Accepted for V1 scope |
| [006](ADR-006-hcsmodel-chunk-container.md) | HCSMODEL chunk container, versioning and checksums | Accepted as EXPERIMENTAL |
| [007](ADR-007-api-mcp-security.md) | API and MCP security boundaries | Accepted (M3) |
| [008](ADR-008-static-crt-single-exe.md) | Static CRT and the single-EXE target | Accepted as a staged plan; the single-EXE claim is NOT made |
| [009](ADR-009-gui-embedded-web-ui.md) | GUI toolkit: embedded local web UI served by the loopback API | Accepted, implemented |
| [010](ADR-010-benchmark-methodology.md) | Benchmark methodology and regression thresholds | Accepted |
| [011](ADR-011-coding-agent-integration.md) | Coding agent integration (prime-agent sidecar) | Accepted for the policy layer, provider document, launch plan and process supervisor; a real agent session is NOT RUN |
| [012](ADR-012-train-rsi-loop.md) | HCS Train and the RSI loop (LoRA, datasets, eval gate) | Accepted for dataset validation, report ingestion, eval gate, adapter book and pipeline; no training run exists |
| [013](ADR-013-web-demo-webgpu-wasm.md) | HCS Studio Web (browser-only WebGPU/WASM demo) | Accepted, implemented in `web/` |

AGENTS.md section 4.2 lists ADR-001 to ADR-010 as the minimum set; ADR-011 to ADR-013 were added with the V1.1 scope
(AGENTS.md section 26).
