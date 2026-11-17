# M2 llama adapter evidence

Build: `cmake --preset windows-msvc-release-vulkan -B build/llama-adapter-rv "-DHCS_EXCLUDE_MODULES=storage;memory;scheduler;telemetry;engines/diffusion;api;mcp;gui"`
then `cmake --build build/llama-adapter-rv --parallel 8` (logs: configure-rv.log, build-rv*.log). llama.cpp pin d3c3e242.
Model: E:\AILOCALTRAIN\data\gguf\qwen3-0.6b-q4_k_m.gguf (SHA-256 4ea79412...37ae7c, see M1/llama/model-sha256.txt).
Real tests: HCS_TEST_GGUF=<model> HCS_TEST_EVIDENCE_DIR=<this dir> build/llama-adapter-rv/bin/hcs_test_llama_engine.exe
- llama-engine-integration.txt : 16/16 passed (6 always-run error tests + 10 real-model tests incl. CPU+Vulkan)
- llama-adapter-results.jsonl  : machine-readable metrics / cancel latency / 10x load-unload memory series
- cli-chat-*.{out,err,json}    : `HCSInfer chat` on cpu and vulkan
- ctest-all-no-model.txt       : ctest without HCS_TEST_GGUF => real-model tests are SKIPPED with "NOT RUN" message
Caveat: first run of the suite (other engineers were compiling concurrently, RAM/commit pressure) failed 9 tests with
ErrorOutOfHostMemory / CPU_REPACK alloc failures and one access violation after a Vulkan OOM; the same binary passed
16/16 once the machine was quieter. Throughput numbers are NOT controlled benchmarks (busy machine, cold Vulkan pipeline
creation in first request, no thermal settling).
