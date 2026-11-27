# CI and release workflows

All workflows live in `.github/workflows/`. Actions are pinned to full commit SHAs (the trailing comment is the release
tag); `.github/dependabot.yml` proposes weekly bumps as one grouped PR. Every pinned action runs on Node 24.

| Workflow | Trigger | What it proves | What it does not prove |
|---|---|---|---|
| `pr.yml` (`pr-quick-gate`) | pull request, push to `main` | CPU Release, Debug and **Release static-CRT** (the shipped preset) configure, build and pass every model-free test; format, clang-tidy, web, python, SBOM, version consistency, actionlint | Vulkan/Radeon execution, real models (skipped = NOT RUN) |
| `pr.yml` POSIX legs | same triggers, plus push to `ci/**` and `release/**` | `linux-gcc`, `linux-clang` (ubuntu-24.04), `linux-arm64` (ubuntu-24.04-arm, preset `linux-arm64-release`), `macos-arm64` (macos-15): configure with Werror ON, build, and the `portable` ctest set (labels `unit`, `contract`, `parser`, `kernel-cpu`, the test presets' filter); `linux-vulkan-smoke`: `linux-release-vulkan` build with the pinned LunarG glslc/spirv-val, then `HCSInfer doctor --json` must enumerate a Vulkan device (Mesa lavapipe) | lavapipe is a CPU Vulkan implementation: no Radeon 680M, no real-model inference (skipped tests = NOT RUN) |
| `release.yml` | tag `v*`, manual | static CPU + Vulkan EXE, imports, restricted-env run, wheels, web dist, checksums, SBOM, draft release | real hardware, clean machine |
| `wheels.yml` | called by `release.yml`, manual | platform wheels + sdist (see the file header) | |
| `hardware.yml` | manual only, and only if `HW_RUNNER_ENABLED=true` | real Radeon 680M / real-model gates | never executed so far (NOT RUN) |

## Release workflow rules

* **version-consistency** runs first: tag == CMake `HCS_VERSION` + `HCS_VERSION_SUFFIX` == `python/pyproject.toml`
  (PEP 440: `-rc.1` -> `rc1`) == `web/package.json`. `scripts/check-version.ps1` is the single implementation and is also
  run by the PR gate. It exports `version` and `prerelease` (true when the suffix is `-alpha/-beta/-rc`).
* **Pre-release** is derived from that version, never hard-coded. Notes come from `docs/release-notes-<version>.md`
  (the job fails if the file is missing).
* **Stable tags** (no suffix) require both `HCSInfer.exe` and `HCSInfer-vulkan.exe`: `exe-vulkan` is then not
  `continue-on-error`, the release job does not run without it, and the assemble step asserts both files. For `-rc`
  tags the Vulkan build may be missing; the draft then lists only what exists and the step summary says so.
* **Existing draft / tag**: the job looks the tag up with `gh release list`. A draft is updated (`upload --clobber`,
  stale assets deleted, notes/title/pre-release flag refreshed). A published release is never modified (the job fails).
  Otherwise a new draft is created. Asset paths are passed as an explicit list, not a glob.
* The workflow never publishes, never pushes to PyPI and never deploys.

## Hardware runner (opt-in)

`hardware.yml` has no schedule and no tag trigger, because without a registered self-hosted runner those runs queue
forever. To enable it: register a dedicated runner (labels `self-hosted, windows, x64, radeon-680m`), set the
repository variable `HW_RUNNER_ENABLED` to `true`, optionally set the model path variables (`HCS_TEST_GGUF`, ...), then
start it with *Run workflow* on `main` or a `v*` tag. Pull-request events never reach it.

## POSIX jobs

* `macos-arm64` is `continue-on-error: true` until proven (macOS is best effort in V1.1); see the evidence README of the bring-up
  (`docs/test-evidence/final-v110/w2-linuxci/README.md`) for the hosted run that decides whether it becomes blocking.
* The Vulkan SDK compiler is **not** taken from apt: Ubuntu 24.04's `glslc` rejects `GL_EXT_integer_dot_product`. The job downloads the
  LunarG SDK tarball for the pinned `VULKAN_SDK_VERSION`, verifies `VULKAN_SDK_SHA256` (the value LunarG publishes at
  `https://vulkan.lunarg.com/sdk/sha/<version>/linux/vulkan_sdk.tar.xz.txt`), keeps only `glslc` and `spirv-val`, and caches them.
  Bump version and hash together, deliberately.
* ccache is wired in through `CMAKE_*_COMPILER_LAUNCHER` on the command line (presets stay launcher-free).
* Tests that skip because no model/GPU exists are counted and reported as NOT RUN in the job summary.

## Hosted-runner caveat: TEMP is on C:

The product refuses to write on `C:` (storage guard). On hosted runners the system TEMP is on `C:`. Tests that call
guarded commands (for example `merge`) must create their directories with `hcs::test::safe_temp_base()` from
`tests/support/temp_dir.hpp`, which falls back to a directory under the (non-`C:`) CTest working directory.

## Verification status

Workflow files are validated locally with actionlint 1.7.12 (0 findings). Hosted execution of the POSIX jobs: see the run table in `docs/test-evidence/final-v110/w2-linuxci/README.md`.
