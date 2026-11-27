# w2-release evidence (2026-11-27)

Base: branch `release/v1.1.0-stable` at `b8c10f9` (the worktree was fast-forwarded to it before any change).

| Item | Result | Evidence |
|---|---|---|
| actionlint 1.7.12 on all of `.github` (release, wheels, pypi, pr, hardware) | 0 findings, exit 0. The `shellcheck` integration was disabled (binary not installed here): shell inside the workflows is NOT shellchecked | `actionlint-verbose.txt` |
| SBOM: purl on every component (15 of 15); generation fails if one is missing | PASS with pwsh 7 on Windows; the Linux `osv-scan` job runs the same script (hosted run NOT RUN) | `sbom-purl.cdx.json` |
| osv-scanner 2.7.1 (official `osv-scanner_windows_amd64.exe`; its sha256 equals the release's `osv-scanner_SHA256SUMS`, which is the same origin as the binary; the linux_amd64 value pinned in release.yml comes from that file) | ran on the SBOM: 15 packages, 4 filtered as local/unscannable, 0 vulnerabilities. This result is NOT meaningful for the native pins: osv-scanner reads `pkg:github` purls as ecosystem "GitHub Actions". Source scan (`-r .`): the repository has no lockfiles, "No package sources found" | `osv-scanner-sbom.json` |
| OSV by commit (`scripts/osv-gate.ps1`, api.osv.dev querybatch for the 11 pinned 40-hex commits) | **FAIL (exit 1)**: 7 findings, 5 waived with justification in `osv-allowlist.json`, 1 LOW, **1 BLOCKING: llama.cpp CVE-2026-133710** (hrm_text loader heap out-of-bounds write, fixed in b12556; the pin is older; CVSS v4 so severity is UNKNOWN = fail-closed). Waived: cpp-httplib CVE-2026-102944, CVE-2026-54919, CVE-2026-77358; llama.cpp CVE-2026-137057, CVE-2026-137567 (justifications checked by grep of `src/` and the CMake files, not by binary inspection) | `osv-gate.json`, `osv-gate-console.txt` |
| Gate self-tests: allowlist entry with a short reason, and one with an expired date | both make the script throw | session log only |
| `scripts/package-posix.sh` with a fake binary (Git Bash, GNU tar) | PASS: archive built, two runs byte-identical, a malformed `--version` is rejected with exit 2. Licence texts absent in this worktree (no submodules): reported as warnings | session log only |
| `scripts/release-build-posix.sh --verify-only` on the w1-posix gcc-15 Linux binary in WSL2 (Ubuntu, glibc 2.43) | the gate works: NEEDED = ld-linux, libc, libm accepted; it correctly FAILS with `needs GLIBC_2.43, above the 2.28 floor` (a build on this host cannot be manylinux); with `--glibc-max 2.50` it passes and `--version` / `doctor --json` run | `verify-only-glibc-floor-fail-BUILD-INFO.txt` |
| Hosted runs of release.yml jobs exe-linux-x86_64, exe-linux-aarch64, exe-macos-arm64, osv-scan, wheel-plan, bin-win-amd64, and the changed release job | **NOT RUN** (nothing was pushed; Actions cannot run from this worktree) | |
| The manylinux_2_28 container build, the Linux Vulkan SDK install inside it, the macOS build with deployment target 11.0 | **NOT RUN** | |
| Shell scripts: `bash -n` | PASS; shellcheck NOT RUN | |

Image pins (resolved from the quay.io API on 2026-11-27): `manylinux_2_28_x86_64:2026.11.21-1@sha256:55a36963...ec53`,
`manylinux_2_28_aarch64:2026.11.21-1@sha256:126d9eea...8246`.
