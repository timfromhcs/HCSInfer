# ADR-010: Benchmark methodology and regression thresholds

- Status: Accepted (2026-11-17). Implements AGENTS.md sections 16.6 and 21.
- Date: 2026-11-17

## Context
Existing numbers (`docs/test-evidence/M1/llama`, `M6-storage`, `M2/image`) were taken on a busy machine with 1-5
samples, so they are provisional. Windows file caching makes warm runs look like algorithm gains. CPU and iGPU share
memory bandwidth.

## Decision
Benchmark IDs B0-B8 are as in AGENTS.md 16.6. Every published result is a JSON file (schema version 1, fields in
AGENTS.md 21.1) plus a short README under `docs/test-evidence/<milestone>/`, produced by one command recorded with it.

Rules:
1. Same machine, driver, build, commit, model hash, quantization, prompt, context, seed and output size for any
   comparison; only one variable changes (backend, load mode, codec, chunk size, kernel flag).
2. Cold and warm are separate benchmarks, labelled in `cold_or_warm`. "Cold" needs a verified cache-miss method
   (reboot, unbuffered read or standby-list flush by the user); otherwise the label is `uncontrolled` (as in the
   current storage JSON) and the number is not an improvement claim.
3. At least 1 warm-up and 5 measured iterations; report median and p90 and keep raw samples. Best-of-N is not allowed.
4. Quiet machine: no concurrent builds or tests; record power plan, thermal state (and throttling if observable), RAM
   and other significant processes. If those are not recorded the result is `provisional`.
5. Report time-to-ready, TTFT, prompt tokens/s, decode tokens/s, image wall time and steps/s, peak RAM/commit, Vulkan
   memory, HDD MiB/s, decompression MiB/s and CPU use where applicable; unmeasured fields are `null`, never zero.
6. A different model hash or quantization is never a backend comparison.
7. Regression policy: a repeated-run median regression above 5% against the previous recorded result for the same ID
   and environment raises a warning in the nightly job; a single noisy run never fails a build. Correctness, crash,
   leak and security failures are hard failures. Performance PRs include baseline and new result from one environment.
8. Claims in README/release notes cite the JSON and state machine, driver, model hash, quantization and context.

## Rejected
Single-run "average" numbers; comparing cold with warm; synthetic microbenchmarks as product claims.

## Evidence / open items
`HCSInfer benchmark` is not implemented; `hcs_storage_bench` (`tools/benchmark-runner`) writes schema-v1 JSON for
storage. A controlled harness for B0-B3 and B7 is NOT RUN. The self-hosted workflow (`.github/workflows/hardware.yml`)
collects doctor and ctest evidence but does not yet publish benchmark JSON.
