# LFM3-Omni-2B Q8_0 as the default local LLM - evidence (task LFM3)

Date: 2026-11-17. Machine: Windows 11 build 10.0.26200.9974, AMD Ryzen 7 7735HS, Radeon 680M (device 0x1681, Vulkan
API 1.4.315, driver `26.6.1`), 21,245,952,000 bytes RAM, MSVC 19.44.35228 x64, CMake 4.4.0, Ninja 1.13.2, Release builds.
HCSInfer sources: base `e0ff7d4` + the working-tree changes of this task (the benchmark JSONs record `a7527e938029+dirty`).
Builds: CPU `build/lfm3-unit` (preset `windows-msvc-release`), Vulkan `build/lfm3-vk` (preset
`windows-msvc-release-vulkan`). Statuses: PASS / FAIL / BLOCKED / NOT RUN.

## Result summary

| Gate | Status | Evidence |
|---|---|---|
| Does the pinned llama.cpp `d3c3e24` support architecture `lfm3` and the omni mmproj? | **PASS - no pin bump needed** | `llama-arch.cpp` defines `LLM_ARCH_LFM3`; `tools/mtmd` has `PROJECTOR_TYPE_LFM3VL`/`LFM3A`; the *upstream* `llama-completion` / `llama-mtmd-cli` built from the same commit (`build/llama-vulkan`) load the Q8_0 model + mmproj and answer on Vulkan (`upstream-mtmd-cli-vulkan.log`). The pin stays `d3c3e242c16b8cbf51400fad410cd69b22d64f35` (see `external/dependency-lock.json`), so there is no compatibility change to regress against |
| Regression on the existing real-model suites (qwen3-0.6b text, SmolVLM2-500M vision), CPU + Vulkan | **PASS** | `regression-llama-engine-{cpu,vulkan}.log`: CPU 14 passed / 3 Vulkan tests skipped (CPU build), Vulkan 16/16; `regression-llama-vision-{cpu,vulkan}.log`: CPU 9 passed / 1 skipped (Vulkan test), Vulkan 10/10 |
| Full ctest on the CPU build, no model variables set | PASS except one unrelated test | `ctest-cpu-no-models.txt`: 468 of 469 pass or skip, 1 FAILED = `GuiAssets.EmbeddedBytesAreIdenticalToSourceFiles` - most likely because another agent edited `src/gui/assets` after this binary was built (embedded bytes vs source differ; `git status` shows those files modified); not part of this task. Real-model tests are skipped (NOT RUN) in that run by design |
| Model download + sha256 | **PASS** | below |
| Chat (greedy), deterministic, CPU and Vulkan, through `HCSInfer serve` | PASS | `e2e-lfm3-{cpu,vulkan}.json/.log` |
| Streaming (SSE) | PASS | same files |
| Cancellation (client disconnect mid-stream, engine serves again) | PASS | same files (`cancel_recovery_s` 1.35 CPU, 0.34 Vulkan) |
| Vision (HTTP `image_url` data URI, SSE, rejects, MCP `hcs.vision.analyze`) | **PASS 27/27 on both builds** | `e2e-vision-lfm3-cpu.log`, `e2e-vision-lfm3-vulkan.{log,json}` (the existing `tests/e2e/vision_real/e2e_vision.py` with LFM3 as the VLM) |
| Tool calling over `/v1/chat/completions` (`tools`, `tool_calls`, stream) | **PASS (experimental)** | implemented in `src/api/chat_tools.cpp`; 19 unit + 12 contract tests; real-model proof inside `e2e-lfm3-*.json` |
| Held-out agentic eval (40 tasks) baseline | **recorded: 37/40 = 0.925** on CPU and on Vulkan (identical per-task results) | `agentic-eval-{cpu,vulkan}.json`, `agentic-responses-*.jsonl` |
| Quick benchmark (AGENTS.md 21.1 JSON), CPU vs Vulkan | PASS (one controlled run each + a repeat) | `benchmark-quick-*` |
| Audio in/out of LFM3-Omni (ASR, TTS, real-time voice), video input | NOT RUN / out of scope | only text + image are wired in HCSInfer V1 (`MTMD_VIDEO=OFF`, no audio path) |

## Model files and licence

Download source: `https://huggingface.co/LiquidAI/LFM3-Omni-2B-GGUF` (public, no token used), `resolve/main/<file>`. Stored on
`D:\HCSModels\text\LFM3-Omni-2B\` (the model drive used by the other evidence, outside the repository; nothing was copied
to an SSD cache).

| File | Bytes | SHA-256 (computed locally = Hugging Face LFS `oid`) |
|---|---|---|
| `LFM3-Omni-2B-Q8_0.gguf` | 1,846,240,768 | `59118f661eaacb93d204723d76355b53347f0940b508c11412f6290fd49158d4` |
| `mmproj-LFM3-Omni-2B-Q8_0.gguf` | 506,842,528 | `8a25f9d1931244429de404fccacfb7fb933970a36aa5688d2f5069f29c6b27b1` |
| `LICENSE` | 10,574 | `4d28ca14dedc0b3d0fcc2b3339f0e79931faa33874f3d24f522183a8fc70068c` |

Licence: **LFM Open License v1.0** (`license: other`, `license_name: lfm1.0`). Apache-style grant, but section 5 limits
*commercial* use to legal entities with annual revenue below **US$ 10,000,000** (the threshold does not apply to qualified
non-profits for non-commercial/research use). The weights are **not** redistributed by HCSInfer (kept outside the repo and the
EXE); a user must accept the licence themselves. Open decision for the project owner: shipping this model as the *default*
in a commercial product needs a licence check against that threshold.

## What was changed

* `src/engines/llama/llama_engine.cpp`: for `general.architecture == lfm3` the engine adds what the model's chat template adds
  and the legacy `llama_chat_apply_template()` ChatML rendering lacks - the BOS token (`<|startoftext|>`, the vocab does not
  auto-add it) and the empty think block (`<think></think>`) after the assistant header. **Found on the real model**: without
  the BOS the model drifts into audio output (`<|audio_start|>:`) on ordinary text prompts (first e2e run failed the counting
  check; the upstream CLI with a hand-written prompt containing the BOS counts correctly, without it prints `: [end of text]`).
* `src/api/control_plane.cpp`, `include/hcs/api/control_plane.hpp`: multi-turn / tool requests on an `lfm3` model use the
  `chatml-lfm3` prompt (BOS + `<think></think>`); `ChatParams::tools` forces ChatML rendering (the engine's legacy path cannot
  express tools) and allows `tool` role messages.
* New `include/hcs/api/chat_tools.hpp` + `src/api/chat_tools.cpp`, hooks in `src/api/http_api.cpp` (route `chat`): OpenAI tool
  calling. Request side: `tools` (<= 64, <= 64 KiB, name `[A-Za-z0-9_.-]{1,64}`, unique), `tool_choice` `auto`/`none`,
  assistant `tool_calls` history (string-encoded `arguments`), `tool` messages, `content: null` on assistant tool-call
  messages. The prompt follows the LFM3/Qwen3-Coder template: `<tools>` JSON lines in the system turn, `<tool_call><function=..>
  <parameter=..>` in assistant turns, `<tool_response>` in tool turns. Response side: the generated text is parsed (XML form and
  Hermes JSON form) into `tool_calls` with schema-typed arguments, `finish_reason: "tool_calls"`, in the non-stream response and
  as an SSE `delta.tool_calls` chunk (text that may start a `<tool_call>` is held back so no tag text leaks into `content`).
  `tool_choice` `required` / a named function and legacy `functions` are **501**, never silently ignored (nothing constrains the
  sampler to emit a call). Tool output cannot forge turns: the control plane's control-token neutraliser still runs over every
  message (contract test `ControlTokensInToolOutputCannotForgeATurn`).
* Tests: `tests/unit/api_chat_tools` (19), `tests/contract/api_chat_tools` (12, incl. the BOS/think prompt), the
  `UnsupportedFeaturesAre501...` assertion in `tests/contract/api` updated (an empty `tools` array is no longer a 501; a forced
  `tool_choice` and `functions` still are), `tests/e2e/llama_agentic_real` (ctest wiring: scorer self-test always, real-model run
  SKIPPED/NOT RUN unless `HCS_TEST_LFM3` is set), `tests/eval/agentic/*` (eval set, scorer, e2e script).

## Measured values (one machine, warm OS cache; not a thermal-controlled study)

Benchmark command (same parameters as the qwen3-0.6b B0/B1 runs in `../benchmark/`: ctx 4096, 64 new tokens, greedy, seed 42,
1 warm-up + 3 measured, one fixed 223-token prompt; prompt text is not logged):

```
build\lfm3-unit\bin\HCSInfer.exe benchmark --suite quick --model <LFM3-Omni-2B-Q8_0.gguf> --backend cpu    --force --export <json>
build\lfm3-vk\bin\HCSInfer.exe   benchmark --suite quick --model <LFM3-Omni-2B-Q8_0.gguf> --backend vulkan --force --export <json>
```

| Metric (warm medians) | CPU, 8 threads | Vulkan (Radeon 680M, 25/24 layers offloaded) |
|---|---|---|
| **decode tokens/s** | **18.8** (p90 18.9) | **35.9** (p90 37.5) |
| **TTFT** | 2,790 ms | **438 ms** |
| prompt tokens/s as reported | 80.0 | 18,116 (see note) |
| model load (warm) | 391 ms | 982 ms |
| peak process working set | 2,057 MB | 514 MB |
| peak process commit | 528 MB | 2,790 MB (UMA allocations show up as commit) |
| wall time per 64-token generation | 6,126 ms | 2,231 ms |
| files | `benchmark-quick-cpu-lfm3-omni-2b-q8_0-quiet.json` | `benchmark-quick-vulkan-lfm3-omni-2b-q8_0-quiet.json` |

Vulkan is ~1.9x the CPU decode rate and ~6.4x faster to the first token for this model.

Notes and caveats (read before quoting any number):

* The files without `-quiet` (`benchmark-quick-{cpu,vulkan}-lfm3-omni-2b-q8_0.json`, `...-cpu-...-run2.json`) were recorded while
  other agents were compiling/running on the same machine (system CPU load was 62% right before the `-run2` run). They are kept for honesty; the CPU
  decode there was 17.2 / 14.4 t/s. The `-quiet` runs started at 4-6% system CPU load.
* The "prompt tokens/s" figure on Vulkan (18,116) is not consistent with the TTFT (223 tokens in 438 ms would be ~510 t/s): the
  Vulkan submission is asynchronous and the per-iteration prompt timer stops before the GPU has finished. The same artefact exists
  in the qwen3-0.6b B1 record. **Use TTFT**, not that field, for Vulkan prefill.
* `cold` in these records means process-cold (first load in the process), not page-cache-cold: `"file_cache": "not_controlled"`.
  The first read of the model from `D:` after the download (cold file cache, upstream `llama-completion`, Vulkan) took
  **111.7 s** to load (`load time = 111738 ms`, ~16 MB/s), the same slow-first-read behaviour already documented for the SD model.
* `e2e-lfm3-*.json` contain single-sample smoke timings (e.g. streaming TTFT 0.091 s and ~36.6 t/s on Vulkan, 0.431 s / 13 t/s on
  CPU while the machine was loaded). They are not benchmarks.
* Memory: the file is 1.85 GB; with 4096 context the engine reports `resident_bytes` 1,837,991,168. The model's native context is
  128,000 tokens - HCSInfer keeps the 4096 default (a 128k KV cache would not fit; upstream's default `n_ctx = 128000` is visible in
  `upstream-*` logs).
* Quality note (not a benchmark): the HCS vision answer to the generated red-square image was "The large square is red." (94
  prompt tokens); upstream's `llama-mtmd-cli` described the white background as "light gray" for the same image - one sample each.

## Agentic eval baseline (LFM3-Omni-2B Q8_0, greedy, through `/v1/chat/completions` with `tools`)

`tests/eval/agentic/tasks.jsonl` (40 tasks, sha256 `9080875e782699913bcf20d0e630b3083a78a8f02b8372464ce4b9cbcebfcf26`), scorer
`tests/eval/agentic/score.py` (self-test: all gold answers accepted, empty answers rejected for all 40). CPU and Vulkan produced
identical per-task outcomes.

| Category | Passed | Notes |
|---|---|---|
| single (one obvious tool call) | 12/12 | |
| select (pick the right tool of several) | 6/6 | |
| typed (int / bool / array arguments) | 4/4 | |
| nocall (must answer without a tool) | 4/4 | |
| followup (answer using a tool result) | 3/3 | |
| **parallel** (two independent calls in one turn) | **0/3** | the model emits one call per turn (verified with the upstream CLI and a hand-built prompt: one `<tool_call>` then end of text); a real model limitation, not a parser bug |
| coding (8 small Python functions, executed against asserts) | 8/8 | restricted subprocess, import allow-list |
| **total** | **37/40 = 0.925** (tool use 0.906, coding 1.000) | |

Caveats: the set is small and mostly easy (a ceiling is near); single sample at temperature 0; the coding tasks are
LeetCode-style one-function problems, not repository-scale work.

## Reproduce

```
python -I tests/eval/agentic/score.py --selftest
python -I tests/eval/agentic/e2e_lfm3.py --exe build/lfm3-vk/bin/HCSInfer.exe --model D:/HCSModels/text/LFM3-Omni-2B/LFM3-Omni-2B-Q8_0.gguf --evidence docs/test-evidence/lfm3 --tag vulkan
python -I tests/e2e/vision_real/e2e_vision.py --exe build/lfm3-vk/bin/HCSInfer.exe --vlm D:/HCSModels/text/LFM3-Omni-2B/LFM3-Omni-2B-Q8_0.gguf --evidence <dir>
```

## Not done / open

* Pin bump: not needed (documented above). Newer upstream (v0.8.0+ for `llama serve` features, audio, video) was not evaluated.
* Parallel tool calls, forced `tool_choice`, `strict` schemas and grammar-constrained calls are not implemented.
* The chat template is reproduced by hand (ChatML + tools block); it is verified against the template text and by behaviour
  (37/40), not by rendering the Jinja template (no Jinja engine in the HCS build; `LLAMA_BUILD_COMMON=OFF`).
* Audio (ASR/TTS/voice), video, multi-image and multi-turn image chat: NOT RUN.
* Fine-tuning on agentic traces / RSI loop: not part of this task; this eval set is the held-out measure to use for it
  (never train on `tests/eval/agentic`).
