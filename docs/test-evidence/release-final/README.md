# Release-final evidence (1.0.0-rc.1)

Two layers, both kept:

1. **Wave R final verification** (commit `e8bd522`): `ctest-release-cpu.txt`, `ctest-release-vulkan*.txt`, `format-check.txt`,
   `clang-tidy.txt`, `web-npm-test.txt`, `pytest-wheel.txt`, `release-local.txt`, `release-verify.txt`, `SHA256SUMS.txt`
   (hashes of the files built at `e8bd522`; **superseded** by the audit layer below for the binaries), `sbom.cdx.json`.
2. **Audit-fix verification** (binaries rebuilt from clean commit `ae3bbbd`, embedded in the EXEs as `ae3bbbdc4050` without
   `+dirty`; later commits on the branch only change documentation, evidence and `scripts/package.ps1`):

| File | What it is |
|---|---|
| `audit-ctest-static-cpu.txt` | full CTest on `windows-msvc-release-static` (the shipped CPU EXE's build), real models set: 816 registered, 806 passed, 4 skipped (Vulkan-only, NOT RUN on a CPU build), 6 failed - all `out_of_memory` refusals by the memory budget (two real-model tests running in parallel under `-j2`) |
| `audit-ctest-static-cpu-rerun-failed6.txt` | the six failed tests (7 CTest entries: all `ShutdownReal.*` and four `e2e_*_real`) re-run serially with `-j1`: 7/7 PASS. Net: 812 passed, 4 skipped, 0 failed |
| `audit-ctest-static-vulkan.txt` | full CTest on `windows-msvc-release-static-vulkan` (the shipped Vulkan EXE's build), real models, Radeon 680M: 815 registered, 813 passed, 2 failed (`e2e_api_mcp_real`, `e2e_llama_agentic_real`, `out_of_memory` refusals under `-j2`) |
| `audit-ctest-static-vulkan-rerun-failed2.txt` | the two re-run serially: 2/2 PASS. Net: 815 passed, 0 skipped, 0 failed |
| `audit-clang-tidy.txt`, `audit-format-check.txt` | clang-tidy 0 diagnostics in 69 TUs; clang-format 19.1.5 OK over 221 files |
| `audit-web-npm-test.txt` | `web/` `npm test`: 46/46 |
| `audit-link-check.txt` | `python tools/check_links.py` on the final tree: 0 broken |
| `shipped-binaries/` | the **shipped** EXEs (SHA-256 `d9b1a6bd...` CPU, `a77cf42a...` Vulkan): empty-PATH restricted-environment runs (`--help`, `doctor --json`, a real LFM3-Omni-2B generation, `serve` health 200, GUI 200, every loaded module under the Windows directory) and a quick benchmark per backend |
| `SHA256SUMS-final.txt` | hashes of the final `artifacts/release` set (LF line endings), assembled from branch head `76b75d4` (the portable ZIP manifest records that commit, tree clean; the EXEs are the `ae3bbbd` builds) |
| `audit-release-verify.txt` | `sha256sum -c` of the release set and of the 70 files inside the ZIP (all OK), LF check, both unpacked EXEs `--version` / `doctor --json` exit 0, link check of the unpacked ZIP (0 broken) |

The first-run failures are kept unedited. They are the memory budget refusing a model load while another real-model test held
about 4 GB (RAM free in total was 7-8 GB); the same tests pass when run alone.
