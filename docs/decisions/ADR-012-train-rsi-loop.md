# ADR-012: HCS Train and the RSI loop (LoRA, datasets, eval gate)

Date: 2026-11-17 - Status: proposed (foundations implemented; no training run exists) - Owner: AGENT+TRAIN wave

## Context
The V1.1 addendum asks for fine-tuning the local LFM3-Omni-2B Q8_0 model for agentic tool calls and coding, and a
recursive-self-improvement (RSI) loop, on this machine: Ryzen 7 7735HS (8C/16T, AVX2/FMA3), Radeon 680M (RDNA 2,
Vulkan, shared memory), about 21 GB RAM, external USB HDD for models. AGENTS.md 15 and 0.2 require measured claims.

Verified facts at the pinned llama.cpp commit `d3c3e242c16b8cbf51400fad410cd69b22d64f35`:
- `examples/training/finetune.cpp` (`llama-finetune`) exists but trains with `llama_opt_param_filter_all`, i.e. **all
  parameters**, not LoRA. Its README calls it "technically functional (for FP32 models and limited hardware setups) but
  very much WIP", reports 24 GB for Llama 3.2 1B, says CPU training needs a build without GPU backends, and notes flash
  attention is disabled during training (no backward pass). Full fine-tuning of a 2B model in FP32 does not fit 21 GB
  next to the OS and optimizer state (estimate, not measured), so it is **rejected** for this model.
- llama.cpp can **apply** a LoRA adapter at inference (`llama_adapter_lora_init`, `tools/export-lora`), so an adapter
  trained elsewhere can be served and merged. Training LoRA is therefore an external-trainer job.
- ROCm is not a baseline for the 680M (AGENTS.md 1.3); PyTorch GPU training on this iGPU is not assumed.

## Decision
1. **Trainer:** LoRA/QLoRA through an **external trainer**, run as an explicit, user-started job, never at app
   start. Candidates, to be evaluated by measurement in a later wave: a CPU PyTorch + PEFT run in a venv (bf16/fp32
   base from the model's original safetensors, not the Q8_0 GGUF), or the existing local tooling already on this
   machine. The output is a LoRA adapter converted to GGUF and applied through llama.cpp. HCS owns data validation,
   the eval gate, adapter bookkeeping and rollback; it does not reimplement a trainer.
2. **Feasibility on this machine (all UNMEASURED, to be measured before any claim):** a 2B model with LoRA rank 8-16
   on attention/MLP projections, sequence length 1-2 K, batch 1 with gradient accumulation, on 16 CPU threads and
   about 21 GB RAM is plausibly feasible for thousands of examples but slow (hours to days, order-of-magnitude guess
   from CPU throughput, not a measurement). The 680M is not expected to help. The training set is therefore a
   filtered 5-20 K record subset, and a smoke run (tens of steps, memory + step time recorded) comes before any real
   run. Whether the LFM3-Omni architecture is supported by a given trainer is UNVERIFIED.
3. **Dataset licence policy (enforced in code by `hcs::train::DatasetValidator`):**
   - every record carries `licence` (SPDX), `source` and `generator`; missing or non-allow-listed licences are
     rejected; the default allow-list is MIT, Apache-2.0, BSD-2/3-Clause, ISC, CC0-1.0, CC-BY-4.0, Unlicense;
     share-alike, non-commercial, custom and unknown licences are rejected;
   - **no distillation of Claude output.** A record whose `generator` or `source` names Claude, Anthropic, Opus,
     Sonnet, Haiku or Fable is rejected. The user's wish for an "agentic Fable-Opus" style is met by training on
     openly licensed data and on HCS's own verified-successful trajectories from the local model, not by copying
     Claude's outputs. Closed hosted models' output needs a manual terms review and is not admitted by default;
   - credentials in a record reject it; exact duplicates are dropped; size caps bound every read;
   - the admitted sources are listed in `docs/research/train-datasets.md`; each needs the checklist there.
4. **Eval gate (`hcs::train::evaluate_gate`):** an adapter is kept only if (a) the score on a held-out set improves by at
   least `min_heldout_gain` (default 0.01) and (b) **no** regression suite drops (tolerance default 0), on identical
   held-out size (at least 50 items) and an identical, non-empty regression suite set. NaN, out-of-range, missing or
   extra suites, or too few samples give `Invalid`, which is treated as `Rollback`. The gate is a pure function; the
   evaluation harness that produces the scores (tool-call format validity, BFCL-style call accuracy, small code
   tasks with executable checks, chat sanity) is a later wave and must run the same prompts and seed for baseline and
   candidate.
5. **Keep / rollback bookkeeping:** adapters are versioned files on the model drive, each with a manifest (base model
   sha256, dataset manifest hashes, hyper-parameters, baseline and candidate scores, gate result). "Active adapter" is
   a pointer; rollback is moving the pointer back, never deleting the previous adapter. The base GGUF is never modified.
6. **RSI loop (bounded, human-gated):** the loop is collect -> validate -> train -> evaluate -> gate -> keep/rollback.
   Training data from the agent's own runs is admitted only when an executable check passed (tests green, command exit
   0) and after `DatasetValidator`; the held-out and regression sets are frozen before training and never receive
   training records. Each cycle is started by the user (or an explicit schedule the user enabled), is resource-capped
   (memory budget, wall time), and ends in Keep or Rollback with a logged reason. There is no unattended unbounded
   self-modification and no change to the runtime binaries or the policy layer by the loop.

## Rejected alternatives
- Full-parameter `llama-finetune` of the 2B model (does not fit; no LoRA at the pinned commit).
- Training on Claude/Opus/Sonnet/Fable outputs or "claude-code" trace dumps (forbidden).
- Auto-promoting every trained adapter (no regression protection) or gating on training loss.
- Implementing a trainer inside HCSInfer for V1.1.

## Risks / open items
- Tool-calling quality of a 2B model may stay low even after tuning; the gate protects against regressions, not
  against a lack of gains.
- Held-out leakage between public datasets: dedupe across train/held-out must be added to the harness.
- Real training time and memory on this machine are not measured; the LFM3-Omni weights are not downloaded in this wave.

## Evidence
Implementation and tests: `src/train`, `include/hcs/train`, `tests/unit/train`; research:
`docs/research/train-datasets.md`; results: `docs/test-evidence/agent/README.md`.
