# S2 evidence: real-model E2E and controlled benchmarks (2026-11-27)

Branch `release/v1.1-stable` at `94d879d`. Vulkan build `windows-msvc-release-vulkan`, CPU build
`windows-msvc-release`. Model: LFM3-Omni-2B Q8_0, SHA-256 `59118f66...d49158d4`, identical for both benchmarks.
One real-model activity at a time; temp/caches on D:.

| Gate | Status | Evidence |
|---|---|---|
| B0 CPU full benchmark (`benchmark --suite full`, 5 iterations, median/p90) | **PASS** | `bench-cpu-full.json`: decode **18.97 t/s**, TTFT **2579 ms**, prompt 86.6 t/s, load 362 ms, peak working set 2.09 GB, result_status PASS, 4 prefill + 4 decode sweep points all PASS with raw samples. |
| B1 Vulkan full benchmark (same model, seed, prompt, iterations) | **PASS** | `bench-vulkan-full.json`: decode **35.97 t/s**, TTFT **311.9 ms**, prompt 13,584 t/s, load 924 ms, peak working set 590 MB, result_status PASS, all sweep points PASS. Device 0x1681, driver 26.6.1, Vulkan 1.4.315, 25/24 layers offloaded. |
| B0 vs B1 comparison | **PASS (measured, one machine)** | **1.90x** median decode, **8.27x** median TTFT on Vulkan; same model hash, seed 42, context 4096, 5 measured iterations, cold phase separate from warm. No thermal data, OS file cache not flushed (documented in JSON). |
| HCS Studio on the **Vulkan** build with real models | **PASS** | `studio-shots-vk.txt`: **42 checks, 0 failed** against a real `serve` (log `serve-vulkan.log`): real model load through the Models tab, streamed chat, Stop/cancel, SD1.5 text-to-image and image-to-image, 3 viewport widths, 0 CSP violations, 0 console errors; screenshots in `studio-vk/`. This closes wave C's "Vulkan-engine chat in the UI" gap. |
| HTTP API real inference over a raw socket | **PASS** | S0 CPU/Vulkan CTest e2e (`e2e_api_mcp_real`, `e2e_image_real`, `e2e_vision_real`) plus `serve-vulkan.log` showing 202 load, 200 chat, 200 cancel, 200 image generation and 200 image edit responses. |
| MCP stdio real client, agentic eval | **PASS** | S0 CPU: `e2e_llama_agentic_real` 1/1 (179.84 s) with `HCS_TEST_LFM3`; Vulkan CTest: agentic 52.86 s, MCP benchmark tool in the contract suite. |

## Provenance caveat

Both EXEs were configured earlier at `6bfe3408d052+dirty`; the benchmark JSON therefore records that commit, not the
final branch head. S4 rebuilds the static release EXEs from the clean final tree and records their hashes. No
performance claim is made beyond this exact model/driver/build pair.
