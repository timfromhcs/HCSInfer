# ADR-004: Memory budget and UMA accounting

Status: Accepted (2026-11-17, finalised) with open validation. Implemented in `include/hcs/memory_budget.hpp` (unit-tested).

## Context
Radeon 680M shares system RAM. A BIOS UMA framebuffer is not extra RAM; Vulkan heaps may over-report. CPU and GPU
share bandwidth and capacity.

## Decision
- Application budget = `min(available_physical - os_reserve - safety_margin, vulkan_headroom)` with saturating
  subtraction (never negative, no wraparound). Inputs are live measurements supplied by `hcs_hardware`;
  the core library performs no OS queries.
- A single ledger with one entry per physical region key. The same shared allocation viewed from CPU and Vulkan is
  reserved once (`MemoryPlacement::SharedUma`); a second reserve of the key fails with `AlreadyExists`.
- Categories: weights, KV cache, workspace, Vulkan buffers, vision buffers, diffusion components, runtime, prefetch.
- Reservation fails with `OutOfMemory` before allocation; overflow is treated as OOM. State is unchanged on error.
- Re-measurement may shrink the budget below reserved (`over_committed()`); new reservations are then refused until release.
- Model switch checks `can_fit` before evicting the current model (to be wired in the lifecycle manager, M2).

## Rejected
Static constants for VRAM; counting Vulkan "dedicated" and host memory as separate pools on UMA.

## Risks
`VK_EXT_memory_budget` accuracy on the 680M driver is unverified; OS reserve/margin defaults must be tuned from real
runs. Windows may page to the page file regardless of the ledger.

## Evidence
Unit tests: `tests/unit/core/test_memory_budget.cpp`. Live inputs: `HCSInfer doctor` reports RAM and GPU memory segments (`docs/test-evidence/M0/doctor.json`: local budget about 14.1 GB, shared system memory about 10.6 GB, dedicated 4.27 GB in the adapter report) - these are separate counters on UMA and must not be summed. The llama run showed a default context of 40960 allocating a 4480 MiB KV cache (`M1/llama`), and a first adapter run under memory pressure failed with host OOM (`M2/llama-adapter-README.md`). NOT RUN: validating the ledger against measured peak RAM of real loads, OOM injection, `VK_EXT_memory_budget` accuracy.
