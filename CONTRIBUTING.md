# Contributing

HCSInfer follows `AGENTS.md` (binding). Short version:

1. Small, reviewable changes; docs and tests in the same change.
2. Build + test before you push: `.\scripts\build.ps1 -Preset windows-msvc-debug -RunTests`.
3. HCS-owned targets must be warning-clean at `/W4` (warnings are errors: `HCS_WARNINGS_AS_ERRORS=ON`).
4. Never weaken or delete a failing test to get green; fix the cause.
5. No fake features: a stub is documented as `Planned`, never presented as working.
6. Dependencies are pinned to exact tags/commits; update them only in dedicated compatibility changes.
7. No model weights, secrets, prompts or personal data in the repository.
8. Format with `clang-format` (`.clang-format`): `.\scripts\format.ps1` checks, `-Fix` rewrites (see `docs/build.md` for the current baseline).
9. Pull requests never run on the self-hosted hardware runner; hardware gates run on `main`/tags (`.github/workflows/hardware.yml`). Report results as PASS / FAIL / BLOCKED / NOT RUN.

## Adding a module

See "Module conventions" in `docs/build.md` (one `src/<module>/CMakeLists.txt`, target `hcs_<module>`,
tests in `tests/unit/<module>/CMakeLists.txt`; no top-level edits for the standard module names).

## Commit messages

Imperative subject line, body explains why. Reference the AGENTS.md milestone (M0-M10).
