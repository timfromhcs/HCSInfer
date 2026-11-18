# Evidence: MCP `hcs.runtime.benchmark` (real model, CPU)

Status: **PASS** (tool plumbing + one real-model run). This closes the open W1 item "MCP
`hcs.runtime.benchmark` tool still open" in `docs/roadmap/IMPLEMENTATION_PLAN.md`.

## What was run

`HCSInfer mcp stdio` over a real stdio session (legacy handshake), one `tools/call` for
`hcs.runtime.benchmark`:

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"hcs.runtime.benchmark",
 "arguments":{"model":"lfm3-omni-2b-q8_0","suite":"quick","warmup":0,"iterations":1,"max_tokens":16,"seed":42}}}
```

* Binary: `build/windows-msvc-release/bin/HCSInfer.exe` (Release, MSVC 19.44, CPU-only build).
* Model: `D:\HCSModels\text\LFM3-Omni-2B\LFM3-Omni-2B-Q8_0.gguf` (1.72 GiB), id `lfm3-omni-2b-q8_0`
  (from `HCSInfer models scan D:\HCSModels\text`).
* Approved root: `--model-root D:\HCSModels\text`.
* Source state: working tree at `b02075f` + the uncommitted MCP-benchmark change (git_commit in the
  document reads `b02075f174c4+dirty`).
* Date: 2026-11-18.

Raw result: `mcp-bench-real.json` (the full AGENTS.md 21.1 document). Server stderr:
`mcp-bench-real.stderr.log`. Probe driver: `build/mcp_bench_probe.ps1` (keeps stdin open until the
call answers; a one-shot pipe would hit the documented EOF grace and cancel the in-flight run).

## Result (warm phase; the document also carries a `cold` block)

| Field | Value |
|---|---|
| `result_status` | PASS |
| `schema_version` | 1 |
| `suite` | quick |
| `backend` | CPU |
| `model_file` | LFM3-Omni-2B-Q8_0.gguf |
| `cold.load_ms_median` | 4403.7 ms |
| `ttft_ms_median` (warm) | 2573.9 ms |
| `decode_tokens_per_second_median` (warm) | 18.85 t/s |
| `peak_ram_bytes` | 2 593 513 472 (≈2.42 GiB) |
| `context_tokens` | 4096 |

## What this does and does not prove

* Proves the MCP tool exists with a narrow, closed input schema, runs the **shared** benchmark
  runner as a real cancellable job (`benchmark.run`), and returns the AGENTS.md 21.1 document — with
  a real GGUF model on real hardware.
* The numbers are a **single quick run**, not a benchmark baseline; `model_sha256` is
  `unknown (not hashed ...)` because the scan did not hash the file. Treat them as tool evidence, not
  as a B0/B1 comparison.
* The plumbing (schema, validation, error mapping, no-engine honesty, call timeout cancels the job)
  is contract-tested in `tests/contract/mcp/test_mcp.cpp` (`McpTools.Benchmark*`).
