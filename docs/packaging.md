# Packaging and distribution

What is produced, how, and what is (not) proven. Evidence for the numbers below: `docs/test-evidence/wave-b/`.
Decision record: [ADR-008](decisions/ADR-008-static-crt-single-exe.md).

## Artefacts

| Artefact | Built by | Contents | Needs on the target machine |
|---|---|---|---|
| `HCSInfer.exe` | preset `windows-msvc-release-static` | CLI, HTTP API, MCP stdio, embedded web UI, llama.cpp + stable-diffusion.cpp (CPU) | Windows 10/11 x64 only. No Visual C++ Redistributable, no sidecar DLL |
| `HCSInfer-vulkan.exe` | preset `windows-msvc-release-static-vulkan` | the same program with the GGML Vulkan backends linked in | additionally the system Vulkan loader `vulkan-1.dll` (installed by every GPU driver) and a Vulkan 1.2+ GPU for acceleration |
| Portable ZIP | `scripts/package.ps1 -Zip` | both EXEs, licences, `THIRD_PARTY_NOTICES.md`, CycloneDX SBOM, `manifest.json` (with the `dumpbin` import report), `SHA256SUMS.txt` | as above |
| Python wheel `hcsinfer-<v>-py3-none-<plat>.whl` | `python -m build --wheel python` with `HCS_WHEEL_PLAT=<plat>` | the binaries for that platform (`HCSInfer[.exe]`, optional `HCSInfer-vulkan[.exe]`) + the `hcsinfer` console script and the stdlib-only Python client | Python >= 3.9 |
| Python pure wheel `hcsinfer-<v>-py3-none-any.whl` | same, no binary staged | client + launcher only; needs `HCSINFER_EXE` or `HCSInfer` on PATH. Fallback for platforms without a native binary | Python >= 3.9 |
| Python sdist `hcsinfer-<v>.tar.gz` | same | Python code only (no EXE); use with `HCSINFER_EXE` | Python >= 3.9 |
| `hcs-studio-web-dist.zip` | `web/tools/build-dist.mjs` | static WebGPU/WASM demo, ready for a static host | a browser (WebGPU optional) |
| `SHA256SUMS.txt`, `hcsinfer.cdx.json` | `scripts/release-local.ps1` / `release.yml` | checksums over every file, SBOM generated from the real build's CMake cache | - |

Models are never included. The GPU driver is never included.

## Why two EXEs

The Vulkan backend of GGML imports the system Vulkan loader statically. A program linked that way does not start at
all on a machine without `vulkan-1.dll` (a PC without a GPU driver). The CPU EXE has no such dependency and is the
file that is guaranteed to start everywhere; the Vulkan EXE is the same program for machines with a GPU driver. The
Python launcher picks `HCSInfer-vulkan.exe` only when `%SystemRoot%\System32\vulkan-1.dll` exists (override with
`HCSINFER_BACKEND=cpu|vulkan|auto` or `HCSINFER_EXE`). A delay-loaded loader that would make one file cover both cases
was **not** attempted: the ggml registry would still touch Vulkan during start-up, and the failure path could not be
tested on this machine (it has a driver).

## Build and assemble locally

```powershell
.\scripts\build.ps1 -Preset windows-msvc-release-static          # CPU, /MT, GGML_OPENMP=OFF
.\scripts\build.ps1 -Preset windows-msvc-release-static-vulkan   # + Vulkan (needs the Vulkan SDK at build time only)
.\scripts\release-local.ps1                                      # -> artifacts\release\*
```

Deep Vulkan builds exceed `MAX_PATH`; on this machine the worktree is mapped with `subst Y:` (see `docs/build.md`).

Check the import claim yourself (a clean-machine run, which this repository has NOT done, is what would justify the word "single-file"):

```powershell
dumpbin /dependents artifacts\release\HCSInfer.exe        # expect only ADVAPI32 / WS2_32 / KERNEL32
.\scripts\restricted-env-check.ps1 -Exe artifacts\release\HCSInfer.exe -OutJson out.json   # empty PATH, empty directory
```

## Python package

`python/` is a hatchling src-layout project. The git-ignored staging directory `python/src/hcsinfer/bin/` may hold
`HCSInfer`(`.exe`) and `HCSInfer-vulkan`(`.exe`). `python/hatch_build.py`:

* binary staged: wheel tag `py3-none-<plat>` (not pure). `<plat>` is the environment variable `HCS_WHEEL_PLAT`, which must
  be one of `win_amd64`, `win_arm64`, `manylinux_2_28_x86_64`, `manylinux_2_28_aarch64`, `macosx_11_0_arm64`,
  `macosx_10_15_x86_64` (anything else aborts the build). When unset it is derived from the build host.
* nothing staged: pure `py3-none-any` client + launcher wheel (the fallback for every platform without a native binary).

`hcsinfer._exe.find_exe()` finds the bundled binary on every OS and never runs anything on import. It prefers
`HCSInfer-vulkan` only when the system Vulkan loader is present (Windows `System32\vulkan-1.dll`, Linux `libvulkan.so.1`;
macOS never) and `HCSINFER_BACKEND` is not `cpu`. Checks: `mypy --strict`, `pytest` (tag selection and discovery are
unit tested with fake binaries and a monkeypatched platform), `twine check`.

Local build for the host platform from binaries you already have (works on Windows; the `.sh` twin on POSIX):

```powershell
.\scripts\build-python.ps1 -Cpu <path\HCSInfer.exe> [-Vulkan <path\HCSInfer.exe of the Vulkan build>] [-Plat win_amd64]
```
```bash
scripts/build-python.sh --cpu <path/HCSInfer> [--vulkan <path>] [--plat manylinux_2_28_x86_64]
```

Both build in a temporary copy of `python/` so the working tree never contains a staged binary.

### Linux wheels and `auditwheel`

The Linux wheels are tagged `manylinux_2_28_*`, a claim about the glibc floor. `auditwheel repair` is **not applicable**: it
grafts shared libraries into extension modules, and the wheel contains a standalone executable. Instead `wheels.yml` fails
the job when `objdump -T` shows a `GLIBC_` version above 2.28 on the staged binary, prints the `NEEDED` libraries and
`GLIBCXX`, and runs `auditwheel show` in the `quay.io/pypa/manylinux_2_28_*` image for information only. The release
binaries must therefore be built on a glibc <= 2.28 system (for example inside that image).

### Wheel workflow and publishing

`.github/workflows/wheels.yml` (`workflow_call` and `workflow_dispatch`) takes per-platform artifacts named
`hcsinfer-bin-<plat>` (files at the artifact root), builds each wheel, `twine check --strict`s it, installs it in a clean venv
on a native runner and runs `hcsinfer --version`, then uploads `wheel-<plat>`, `wheel-pure` and `sdist`. Publishing is a
separate manual workflow, `pypi.yml`: [pypi-publishing.md](pypi-publishing.md). Hosted verification of the Linux and macOS
jobs is **NOT RUN** (not executable on the Windows development machine); the Windows wheel was built and installed locally.
## CI

* `pr.yml` (every PR): adds a `python` job (mypy --strict, pytest, `python -m build`, `twine check`) and builds + boots the
  web `dist`.
* `release.yml` (tag `v*` or manual): builds both static EXEs, checks their imports with `dumpbin`, runs the
  restricted-environment script, packages, builds sdist + wheel, installs the wheel in a fresh venv, builds the web dist,
  writes `SHA256SUMS.txt` + SBOM and, on a tag, creates a **draft** GitHub release. It never publishes, never pushes to
  PyPI and never deploys. The Vulkan job is `continue-on-error` only for pre-release tags (`-rc`/`-beta`): a hosted
  runner has no GPU and the SDK install is the least-proven step; a stable tag requires both executables. An existing
  draft for the tag is updated in place; a published release is never modified. See `docs/ci.md`.
* Hardware tests (Radeon 680M, real models) are `hardware.yml`, run on a self-hosted machine before a draft is published.
* Workflow files are checked with actionlint (CI job `actionlint`, v1.7.12 with a pinned SHA-256; older evidence: v1.7.7, `docs/test-evidence/wave-b/actionlint.txt`).

## Not done

* Clean-machine test (fresh Windows without redistributable, driver or SDK): **NOT RUN**. The restricted-environment
  script is a proxy, not a replacement.
* Code signing: **NOT RUN** (no certificate). Binaries are unsigned; Windows SmartScreen may warn.
* MSIX / installer (WiX, Inno Setup, MSIX packaging tools): not present on the build machine, **skipped** rather than
  installed from an unverified source. The portable ZIP and the wheel are the install paths.
* `GGML_OPENMP=OFF` is required for a system-DLL-only EXE (vcomp140.dll would otherwise be imported). Its CPU cost was
  measured once in wave B (LFM3-Omni-2B Q8_0, 5 iterations, `benchmark --suite quick`): decode 18.59 vs 18.96 tokens/s
  (-1.9%), prefill 74.3 vs 77.7 tokens/s (-4.4%) against the OpenMP `/MD` build. Single run per variant, see `docs/benchmarks.md`.
* Full CTest on the two new static presets was NOT re-run in wave B (no C++ source changed; the static CPU preset passed
  783/783 in W4 and 814/0 failed in wave G). The static Vulkan preset's tests have never been built.
* The hosted `release.yml` run is done: green on `40bf581` (draft pre-release with the files above); a clean-machine run of those artifacts is NOT RUN.
