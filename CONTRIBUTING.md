# Contributing

HCSInfer follows `AGENTS.md` (binding). Short version:

1. Small, reviewable changes; docs and tests in the same change.
2. Build + test before you push: `.\scripts\build.ps1 -Preset windows-msvc-debug -RunTests`.
3. HCS-owned targets must be warning-clean at `/W4` (warnings are errors: `HCS_WARNINGS_AS_ERRORS=ON`).
4. Never weaken or delete a failing test to get green; fix the cause.
5. No fake features: a stub is documented as `Planned`, never presented as working.
6. Dependencies are pinned to exact tags/commits; update them only in dedicated compatibility changes.
7. No model weights, secrets, prompts or personal data in the repository.
8. Format with `clang-format` (`.clang-format`): `.\scripts\format.ps1` checks, `-Fix` rewrites (see `docs/build.md`).
   Run the static-analysis gate with `.\scripts\static-analysis.ps1 -Scope src -FailOnFindings` (see
   `docs/static-analysis.md`); both are blocking CI jobs.
9. Pull requests never run on the self-hosted hardware runner; hardware gates run on `main`/tags (`.github/workflows/hardware.yml`, never executed so far). Report results as PASS / FAIL / BLOCKED / NOT RUN.
10. The browser demo lives in `web/` (needs only Node, no npm dependencies): `cd web; npm test` and
    `node tests/e2e/boot.mjs` must pass; the CI `web` job runs both.
11. Tests that need a real model skip unless `HCS_TEST_GGUF`, `HCS_TEST_VLM` + `HCS_TEST_MMPROJ` or
    `HCS_TEST_DIFFUSION_MODEL` is set; report those runs as NOT RUN, never as PASS.

## Adding a module

See "Module conventions" in `docs/build.md` (one `src/<module>/CMakeLists.txt`, target `hcs_<module>`,
tests in `tests/<layer>/<module>/CMakeLists.txt`; no top-level edits for the standard module names).

## Commit messages

Imperative subject line, body explains why. Reference the AGENTS.md milestone (M0-M10) or V1.1 wave.
