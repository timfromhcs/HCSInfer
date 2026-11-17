# Held-out agentic eval (tool calling + small coding tasks)

**Never train, fine-tune, or tune prompts on these files.** They are the fixed held-out measure for the local LLM
(LFM3-Omni-2B Q8_0 today) and for any later fine-tune / RSI loop.

| File | What |
|---|---|
| `tasks.jsonl` | 40 tasks, one JSON object per line: `id`, `category`, `tools`, `messages`, `expect` (sha256 `9080875e782699913bcf20d0e630b3083a78a8f02b8372464ce4b9cbcebfcf26`) |
| `score.py` | automatic scorer; live mode (OpenAI-compatible endpoint), `--replay` mode (re-score saved answers), `--selftest` |
| `e2e_lfm3.py` | real-model end-to-end run through `HCSInfer serve` (chat, streaming, cancellation, tool calling) followed by this eval |

Categories: `single` 12 (one obvious call), `select` 6 (choose among tools), `typed` 4 (int/bool/array arguments), `nocall` 4
(must not call a tool), `parallel` 3 (two independent calls in one turn), `followup` 3 (answer from a tool result), `coding` 8
(a Python function, executed against asserts).

Scoring: a task passes or fails (no partial credit). Tool calls must match name and every expected argument (case-insensitive
strings, numeric equality, `contains`/`calc` matchers where the wording is free); the number of calls must match. Coding answers
are run with `python -I` in a temp directory, 5 s timeout, after an AST check that allows only `math re itertools collections
functools string json heapq bisect typing operator` imports and forbids `open/exec/eval/compile/__import__/input` and dunder
attributes. This reduces, but does not eliminate, the risk of running model-written code: run the live eval on a machine you
are willing to run model output on.

```
python -I score.py --selftest                                              # no model: gold answers pass, empty answers fail
python -I score.py --base-url http://127.0.0.1:<port> --token <t> --model <id> --out result.json --save-responses answers.jsonl
python -I score.py --replay answers.jsonl --out result.json               # re-score saved answers
```

Baseline (LFM3-Omni-2B Q8_0, greedy, `max_tokens` 400): 37/40 = 0.925; see `docs/test-evidence/lfm3/README.md`.
Limits of this set: small, mostly easy (near ceiling for a good model), single sample, no multi-step tool loops beyond one
tool result, no repository-scale coding.
