# Model merge (`HCSInfer merge`)

Status: **Experimental**. Linear, SLERP, TIES, DARE and LoRA-into-base are implemented as a streaming, tensor-wise merge on
safetensors and GGUF files with float tensors, unit-tested against NumPy-computed golden values and exercised end to end on a
real 320 MB safetensors file (see "Evidence and its limits"). Loading a merged model into an engine and generating from it is
**BLOCKED** (2026-11-27): the local diffusion GGUFs are quantized / have different tensor sets - SD1.5 + SD-Turbo fails
with `invalid_argument` (1131 vs 1323 tensors), and a quantized self-merge is refused with `unsupported` (exit 10),
proving the float-only guard. Evidence: `docs/test-evidence/final-s1/merge-quantized-*.txt`. A download needs owner approval.

```
HCSInfer merge --method linear|slerp|ties|dare|lora --base <file> --model <file> [--model <file> ...] --out <file> [options]
```

| Method | Inputs | Definition |
|---|---|---|
| `linear` | base + N models, N+1 `--weights` (default equal) | `sum(w_i m_i) / sum(w_i)` |
| `slerp` | base + exactly one model, `--t` in [0,1] | spherical interpolation per tensor; near-parallel or zero-norm tensors fall back to lerp |
| `ties` | base + N task models, `--density`, `--lambda`, N `--weights` | task vectors `m_i - base`, trim to the top-`density` magnitudes, elect sign, disjoint mean, `base + lambda * merged` |
| `dare` | base + N task models, `--drop-rate`, `--seed`, `--lambda` | random drop with `1/(1-p)` rescale of each task vector (deterministic mask from the seed), `base + lambda * sum(w_i d_i)` |
| `lora` | base + one LoRA file, `--weights <strength>`, `--alpha` | `base + strength * (alpha/rank) * (B @ A)` for every targeted tensor; PEFT (`lora_A/lora_B`), kohya `lora_down/lora_up(+alpha)` and llama.cpp `.lora_a/.lora_b` names; kohya `lora_unet_...` underscore names cannot be mapped and are rejected |

Exact definitions and the arithmetic (double per element, one rounding to the output dtype) are in `include/hcs/merge.hpp`.

## Limits (V1.1)

* **Float tensors only**: F32, F16, BF16. For linear/slerp/ties/dare a GGUF with any quantised tensor (Q4_K, Q8_0, ...) is
  rejected (`unsupported`); **quantised formats are not merged**. For `lora`, quantised base tensors the LoRA does not target are
  copied unchanged, while a LoRA that targets a quantised tensor is rejected. Merge in a float format first and quantise
  afterwards with the normal tooling.
* All inputs must share one container (safetensors+safetensors or GGUF+GGUF) and have identical tensor names and shapes.
  Any mismatch fails before a byte is written. Non-float safetensors tensors are copied from the base.
* **Streaming**: tensors are read in bounded chunks (default 4 Mi floats per input); two models are never fully in memory.
  Only TIES needs one whole-tensor magnitude buffer (4 bytes/element), checked against `--max-ram-mib` (default 2048, fail-closed).
* Output: written to `<out>.partial`, renamed on success, so a failed or cancelled run leaves no output. `<out>.merge.json`
  records method, parameters, input sizes and the output SHA-256 (`--no-manifest` to skip). An existing output needs `--force`; the output can
  never be one of the inputs.

## Path safety

Every path passed on the command line becomes an approved location for that one run (like `models scan <dir>`), and the
output is additionally refused on **drive C:**. The merge core (`hcs::merge::run_merge`) takes a `PathGuard`, so a future
API/MCP job can pass the server's approved roots instead; no HTTP or MCP merge route exists yet (the plan's "API/MCP job" part of
D3 is **not implemented**).

## Evidence and its limits

* Unit tests (`tests/unit/merge`): every method against golden values computed by an independent NumPy implementation
  (`gen_merge_golden.py`, generated in a Python venv under `D:\HCSScratch`), chunk-boundary independence, rejection of mismatched
  names/shapes, quantised GGUF, outputs on C:, output rules, cancellation leaving no file, memory-limit fail-closed. CLI contract test:
  `tests/contract/cli/test_cli_merge.cpp`.
* Real files: `HCSInfer merge --method linear` and `--method slerp` with **the same real file as base and model**
  (`flux2-vae.safetensors`, 320.6 MB, 251 tensors, F32) completed in about 6 s each with a peak working set of 46 MB; every
  output tensor equals the input (max abs diff 0.0 for linear and slerp, verified with NumPy). This proves streaming, bounded
  RAM and the container round trip on a real file; it does **not** prove merge quality, because merging a model with itself is
  the identity. Evidence: `docs/test-evidence/wave-b/b3/real_merge*.txt`.
