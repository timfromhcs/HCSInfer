# M1 llama.cpp independent baseline (evidence)

Commit pinned: d3c3e242c16b8cbf51400fad410cd69b22d64f35 (build string d3c3e24). Tools: see tool-versions.txt
(CMake 4.4.0, Ninja 1.13.2, MSVC via vcvars64, Python 3.14, Vulkan SDK 1.4.357.0).

Builds (Ninja, Release, -DLLAMA_CURL=OFF; logs configure-*.log / build-*.log):
- build/llama-vulkan: -DGGML_VULKAN=ON   (717 steps, exit 0)
- build/llama-cpu:    -DGGML_VULKAN=OFF  (386 steps, exit 0)
Binaries: D:\HCSInfer\build\llama-{vulkan,cpu}\bin\{llama-cli,llama-completion,llama-bench,llama-server,llama-mtmd-cli}.exe

Devices: Vulkan build lists `Vulkan0: AMD Radeon(TM) Graphics (14226 MiB, 13515 MiB free)`; CPU build lists none.
Model: E:\AILOCALTRAIN\data\gguf\qwen3-0.6b-q4_k_m.gguf, SHA-256 4ea794120a3b2244a6e8a36354f1aa53e7f0c52b2f1f280dde0d7f522f37ae7c.
E: is a USB drive (IB-1232CL-U3, BusType USB).

Vulkan proof (gen-vulkan-verbose.log): "using device Vulkan0 (AMD Radeon(TM) Graphics)", "offloaded 29/29 layers to GPU",
Vulkan0 model buffer 372.65 MiB, KV 4480 MiB (default ctx 40960 - very large; shows why context must be budgeted), compute 344.77 MiB.
CPU run (-ngl 0, CPU build) has no Vulkan lines.

Generation (llama-completion, seed 42, temp 0, -n 32, 12 prompt tokens; text is low quality for a 0.6B model; CPU and Vulkan outputs differ):
| run | backend | load ms | pp t/s | tg t/s |
| 1 (COLD: first read from USB HDD, also first Vulkan shader init) | Vulkan | 30198 | 17.0 | 75.5 |
| 2 (warm, OS file cache) | Vulkan | 1081 | 254.8 | 106.4 |
| 3 (warm) | Vulkan | 1163 | 335.4 | 120.9 |
| 1 (CPU build; model likely already cached from Vulkan runs - not cold) | CPU | 1807 | 248.0 | 63.6 |
| 2 | CPU | 1358 | 227.7 | 64.4 |
| 3 | CPU | 1391 | 240.6 | 66.4 |
Run-to-run variance is high; 3 samples only.

llama-bench (-p 128 -n 64 -r 5, warm; bench-*.md/json), mean +- sd t/s:
- Vulkan ngl99: pp128 1530.04 +- 95.50, tg64 136.03 +- 0.89
- CPU 8 threads: pp128 416.65 +- 17.90, tg64 67.89 +- 2.93
Not a controlled benchmark (no thermal settling, power plan unrecorded). Provisional.

VLM: BLOCKED. Only mmproj found: E:\Downloads\Ternary-Bonsai-2-9B-mmproj-Q8_0.gguf (qwen3vl_merger, projection_dim 4096,
SHA-256 e0cb6f69...953db). The only matching-family text GGUF is D:\PrimeAgent\Ternary-Bonsai-2-27B-Abliterated-PQ2_0.gguf
(qwen35, n_embd 5120) -> dimension mismatch with the 9B projector; no 9B text model present. No test image with known
redistribution rights available either (only llama.cpp media/matmul.png, a diagram). llama-mtmd-cli built and ready; not run.
Needs: Ternary-Bonsai-2-9B text GGUF (or another supported VLM pair) - download requires user decision.
