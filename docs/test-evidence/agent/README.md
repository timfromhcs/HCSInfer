# Evidence: AGENT + TRAIN foundations (2026-11-17)

Nothing was committed by this wave. No model or dataset was downloaded; no training or agent run happened.

## 1. prime-agent submodule and build
- `git submodule add https://github.com/PrimeIntellect-ai/prime-agent external/prime-agent` -> v0.10.4, commit
  `8980e66f2f07762d62a6c1d73b347a4cac5275cc` (2026-11-17 release commit), licence MIT (`external/prime-agent/LICENSE`,
  Copyright (c) 2025-2026 Prime Intellect Ltd.). New entry in `external/dependency-lock.json`; row in
  `THIRD_PARTY_NOTICES.md`.
- Toolchain: cargo/rustc 1.96.0, host `x86_64-pc-windows-msvc` (already installed; nothing installed). No installer
  script was run.
- Command: `cargo build --release` in `external/prime-agent` with `CARGO_TARGET_DIR=build/agent/cargo`.
  Result: **PASS**, `Finished release profile ... in 10m 22s` (`cargo-build-summary.txt`); the log has only
  `unexpected cfg` warnings from the vendored `crossterm`.
- Artefacts: `prime-agent.exe` 65 864 704 bytes, `pa-daemon.exe` 45 451 264 bytes; `prime-agent.exe --version` ->
  `0.10.4`; `--help` lists `--provider --model --api-key --offline --mode text|json|rpc|acp|daemon`.
- NOT RUN: `cargo test`, `cargo deny`, a session, the Python kernel bootstrap (needs `uv`, present at
  `~\.local\bin\uv.exe`, plus network for the venv), any run against HCSInfer (chat tool calls are 501 in the API today).
- Protocol findings: `docs/research/prime-agent.md`.

## 2. C++ pieces
- `hcs_agent`: `include/hcs/agent/sandbox_policy.hpp`, `src/agent/sandbox_policy.cpp` (command allow-list with pinned
  sub-command and denied args, hard-denied shells/interpreters/network programs, `PathGuard` reuse for absolute and
  `..` paths and cwd, `.git` write guard incl. `GIT~1`, timeout clamp, UTF-8 safe output cap).
- `hcs_train`: `include/hcs/train/dataset_validator.hpp` + `src/train/dataset_validator.cpp` (JSONL schema, required
  `licence`/`source`/`generator`, licence allow-list, forbidden Claude provenance, bounded line reader, size caps,
  depth guard, secret detection, normalised 128-bit dedupe), `include/hcs/train/eval_gate.hpp` + `src/train/eval_gate.cpp`
  (keep only if held-out improves and no regression suite drops; fail closed on invalid input).
- Wired as modules `agent` and `train` in `HCS_MODULE_DIRS` (root `CMakeLists.txt`), libraries built with the
  `hcs_warnings` interface target = `/W4 /permissive- /WX`.
- Tests: `tests/unit/agent` (SandboxPolicy.*), `tests/unit/train` (DatasetValidator.*, EvalGate.*).
- Build (isolated configuration, MSVC 19.44 / Ninja / Release, modules that need the engine submodules excluded
  because they are not initialised in this worktree): `build/agent-cmake`. Result: **33 of 33 tests passed**
  (`ctest-unit.txt`, `build-hcs-targets.txt`), no warnings in HCS targets. One real bug was found by the first run
  (leading whitespace changed the dedupe key) and fixed with the failing test kept.
- Formatting: `clang-format` 19 (VS-bundled) applied to the new files.
- NOT RUN: the full regression suite of the other modules (excluded in this isolated build), clang-tidy, the
  Vulkan build, fuzzing of the validator (only unit-level hostile inputs: 100 KB line, 20 000-deep JSON).

## 3. Docs
`docs/decisions/ADR-011-coding-agent-integration.md`, `ADR-012-train-rsi-loop.md`, `docs/research/prime-agent.md`,
`docs/research/train-datasets.md` (licences read from Hub metadata; generator provenance of several datasets is marked
UNVERIFIED there).
