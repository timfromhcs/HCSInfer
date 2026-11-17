# ADR-003: Vulkan device/queue ownership

Status: Accepted for V1 (2026-11-17, finalised). Kernel-related parts (M8) remain a forward decision.

## Context
Upstream GGML Vulkan creates and owns its own VkInstance/VkDevice/queues. HCS wants (a) truthful device and
heap reporting, (b) its own opt-in kernels later (M8), (c) no cross-queue races.

## Decision
- Detection (`hcs_hardware`) uses a short-lived, separate VkInstance to enumerate devices, features, limits and
  memory heaps, then destroys it before any engine starts. It never submits work.
- During V1 each engine backend (GGML Vulkan) owns its device and queues; HCS does not share VkDevice handles
  with upstream. Exactly one engine context uses the GPU at a time (JobQueue default 1 worker + model lifecycle
  serialisation); the other engine is unloaded or CPU-only.
- HCS-owned kernels (M8) will run through GGML's backend extension points inside the same device, behind a feature flag,
  not through an independent device, unless a measured reason exists.
- Feature selection is by runtime query (subgroup size, fp16, int8, extensions), never by GPU name.
- On `VK_ERROR_DEVICE_LOST`: stop submitting, mark the context `Failed(NonRecoverable)` in `ModelLifecycle`, release host
  resources, recreate only through an explicit reload.

## Rejected
Sharing one VkDevice across upstream and HCS code in V1 (lifetime and synchronisation risk outweigh benefit).

## Risks
Two instances exist briefly (detection then engine); Windows driver quirks on 680M (UMA heap reporting) must be verified on hardware.

## Evidence
`HCSInfer doctor` enumerates the Radeon 680M with its own short-lived instance (`docs/test-evidence/M0/doctor.json`, `vulkaninfo-summary.txt`), and the GGML Vulkan backend then creates its own device for text and diffusion runs (`M1/llama`, `M2/llama-engine-integration.txt`, `M2/image/README.md`; Vulkan0 = AMD Radeon(TM) Graphics, uma 1, fp16 1, warp size 32). Not verified: behaviour under `VK_ERROR_DEVICE_LOST`, concurrent text+diffusion contexts (the facade unloads one before loading the other), validation-layer runs.
