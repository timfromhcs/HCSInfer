# HCSInfer 1.1.0

Status: **TEMPLATE, not final.** The `release` workflow refuses a stable tag (`v1.1.0`) while this file still contains the
block marked `TO BE FILLED BY COORDINATOR`. Everything outside that block is a verified fact with a pointer to its
evidence; everything inside it is deliberately empty until the final commit has been built and tested. Items without
evidence are `NOT RUN` or `BLOCKED`, never `PASS`.

Authoritative gate table: `docs/roadmap/STATUS.md`. Platform support: `docs/support-matrix.md`. Installation:
`docs/install.md`.

## Supported platforms (summary)

| Platform | Binary | Level |
|---|---|---|
| Windows 11 x64, CPU | `HCSInfer.exe` (static CRT, system-DLL-only imports) | supported; verified on one machine (Ryzen 7 7735HS) |
| Windows 11 x64, Vulkan | `HCSInfer-vulkan.exe` (also imports the system `vulkan-1.dll`) | supported; verified on one GPU (Radeon 680M) |
| Linux x86_64, CPU | `HCSInfer-<ver>-linux-x86_64.tar.gz` (built in manylinux_2_28, glibc floor 2.28) | supported as a CPU build; hosted CI runs the portable test set |
| Linux x86_64, Vulkan | `HCSInfer-vulkan` inside the same archive when it was built | **experimental**; loads the system `libvulkan.so.1`; the only Linux GPU-stack evidence is a `doctor` loader check against Mesa llvmpipe; no GGML Vulkan inference run, no real-GPU run |
| Linux aarch64, CPU | `HCSInfer-<ver>-linux-aarch64.tar.gz` | **experimental** |
| macOS arm64, CPU | `HCSInfer-<ver>-macos-arm64.tar.gz` (no Metal) | **experimental** |
| Python | `hcsinfer` wheels `win_amd64`, `manylinux_2_28_x86_64`, plus `manylinux_2_28_aarch64`, `macosx_11_0_arm64` when built; pure wheel; sdist | wheels are only as supported as the binary inside |

Not supported / not done: Metal, ROCm, real-GPU runs on Linux or macOS, the coding-agent sidecar on Linux/macOS (Windows
only), code signing (the binaries are unsigned), PyPI upload (owner-gated, `docs/pypi-publishing.md`), a clean-machine run.

## What changed since 1.1.0-rc.1

- Linux/macOS CPU port (`docs/test-evidence/final-v110/w1-posix/`): `pread` file source, `/proc` hardware probes, dynamic
  Vulkan loader seam. Verified on Linux x86_64 in WSL2: gcc 15 and clang 21 build with `-Werror`; portable ctest set 962 of
  963 passed, 1 environment failure (WSL localhost relay) and 9 skipped; one real LFM3 CPU generation.
- Release pipeline for all platforms (`.github/workflows/release.yml`), multi-platform wheels (`wheels.yml`), deterministic
  POSIX archives (`scripts/package-posix.sh`), CycloneDX SBOM with a purl on every component, an OSV vulnerability gate with
  a justified allowlist (`scripts/osv-gate.ps1`, `osv-allowlist.json`), `SHA256SUMS.txt` with LF endings that is re-checked
  with `sha256sum -c` inside the workflow.

## Known limits (carried over, still true unless the evidence block below says otherwise)

- Vulnerability gate (local run, `docs/test-evidence/final-v110/w2-release/`): of 7 OSV findings against the pinned
  native dependencies, 5 are waived in `osv-allowlist.json` with a source-level justification and expire on 2027-02-28;
  **1 is unresolved and blocks a stable tag: llama.cpp CVE-2026-133710** (heap out-of-bounds write in the `hrm_text` model
  loader, fixed upstream in b12556; the pinned commit is older). Resolving it needs a llama.cpp pin bump, which is a
  separate compatibility change (AGENTS.md 7.2), or an explicit owner decision to waive it. The cpp-httplib pin is also
  affected by advisories fixed in 0.47 to 0.51 (waived only because the vulnerable code paths are not used); bumping it is
  the real fix.
- Hosted runs of the Linux, macOS and wheel jobs: see the evidence block. Until a hosted run exists they are `NOT RUN`.
- Everything listed under "Known limits" in `docs/release-notes-1.1.0-rc.1.md` that has not been re-verified on the final
  commit (merge of two float models, kernel default-enablement, ONNX GPU provider, real trainer / agent / WebGPU / Vercel
  runs, clean-machine run, ASan fuzzing).

## TO BE FILLED BY COORDINATOR FROM EVIDENCE

Replace every placeholder below with a value copied from a file under `docs/test-evidence/`, or with `NOT RUN` /
`BLOCKED` and the reason. Do not paste numbers from memory. Delete this heading when the block is complete (the release
workflow checks for it).

- Release commit / tag / merge commit: `<fill>`
- Hosted `release` run id and result per job (version-consistency, exe-cpu, exe-vulkan, exe-linux-x86_64,
  exe-linux-aarch64, exe-macos-arm64, wheels, web, osv-scan, release): `<fill>`
- Windows CPU + Vulkan full CTest with real models on the final commit: `<fill>`
- Linux x86_64 CPU portable ctest result (count passed/failed/skipped) from the hosted job: `<fill>`
- Linux x86_64 binary gate output (`BUILD-INFO-cpu.txt`: NEEDED libraries, max GLIBC, sha256): `<fill>`
- Linux x86_64 Vulkan binary built: `<fill yes/no>`; real-GPU run: `NOT RUN` unless evidence exists
- Linux aarch64 / macOS arm64: built `<fill>`, portable tests `<fill>`; keep the label **experimental** unless a hosted run
  and a real-machine run both prove otherwise
- Wheel smoke install per platform (hosted wheels.yml run id): `<fill>`
- OSV gate result on the final commit, allowlist entries in force, unresolved findings: `<fill>`
- Benchmark / hardware-gate results used in the notes (B0/B1 JSON paths): `<fill>`
- SHA-256 of every release asset (copy of `SHA256SUMS.txt`): `<fill>`
- Items that stay NOT RUN / BLOCKED in this release: `<fill>`
