# Evidence: wave G - hardening gaps (2026-11-20)

Machine: Ryzen 7 7735HS, ~21 GB RAM, Windows 11 Pro 10.0.26200, MSVC 19.44 / VS 2022, CMake 4.4.0, Ninja. Source: branch
`claude/repo-analysis-v1-stable-d7c553`, built in this worktree with the CPU Release preset (`windows-msvc-release`,
Vulkan off) from `Y:` (short drive, MAX_PATH). Model for the real-model runs: LiquidAI LFM3-Omni-2B Q8_0
(`D:\HCSModels\llm\LFM3-Omni-2B`, already on disk, no download in this wave).

## Result summary

| Item | Status | Evidence |
|---|---|---|
| (a) OOM injection at the allocator boundary | **PASS** (injected); real memory exhaustion NOT RUN | `oom-red-before-fix.txt` (2 of 7 red before the fix) -> 8/8 green in `ctest-release-cpu.txt` |
| (b) USB-disconnect simulation on the storage layer | **PASS** (simulated); physical cable pull NOT RUN | `tests/unit/storage/test_unplug.cpp` (6 tests) in `ctest-release-cpu.txt` |
| (c) Shutdown while idle / loading / generating | **PASS** (fake engine 7 tests; real model 3 tests); Vulkan engine NOT RUN | `shutdown-real-model.txt`, `ctest-release-cpu.txt` |
| (d) Supply chain | **PASS with findings, one fixed** | `npm-audit.txt`, `cargo-audit-prime-agent.txt`, `cargo-licences-prime-agent.txt`, `osv-native-deps.txt`, `licence-reverify.txt`; analysis in `docs/licence-audit.md` Findings 5 and 6 |
| (e) Restricted-environment run of the static /MT EXE | **PASS as a partial proxy**; real clean-machine test NOT RUN | `restricted-env-static-exe.json`, `dumpbin-static-wave-g.txt`, `ctest-release-static.txt`, `clean-machine-availability.txt` |
| Full CPU Release CTest | **PASS**: 814 registered, 778 passed, 36 skipped (NOT RUN: need a model / GPU), 0 failed | `ctest-release-cpu.txt` (before the httplib bump), `ctest-release-cpu-httplib-0.44.txt` (after) |

## (a) OOM injection
* Seam: `MemoryBudget::set_fault_injector` - every `reserve()` and growing `resize()` asks a hook first; a `true`
  answer fails exactly like a real refusal (`OutOfMemory`, ledger untouched). Tests only; no production caller sets it.
* **Defect found and fixed (red -> green):** an engine that threw `std::bad_alloc` surfaced as a generic
  `BackendError`. `error_from_engine_exception()` (include/hcs/result.hpp) now maps it to `OutOfMemory` at all six
  `catch` sites of the control plane (load, chat, vision, image, benchmark x2). `oom-red-before-fix.txt` is the run with
  the old mapping restored: `BadAllocEscapingTheEngineLoadIsAStructuredOutOfMemory` and
  `BadAllocDuringGenerationFailsTheJobAndTheModelStaysLoaded` FAIL (code 4 instead of 5); the other 5 pass because the
  rollback logic already existed.
* Properties asserted: structured `OutOfMemory`; `reserved()` back to 0 (including the weights reservation made before
  a refused KV reservation); the engine is not touched after a refused reservation; model state `Failed` and
  retryable; the next load and chat succeed; 25 refusals in a row leak nothing; a refused model switch leaves no
  stale entry (the previous model has already been evicted at that point - the control plane checks the fit before
  evicting but reserves after; documented here, not changed).
* Not tested: genuine RAM exhaustion, Vulkan allocation failure inside GGML (outside the HCS allocator boundary).

## (b) USB disconnect
* `write_hcsmodel` gained an overload over any `ByteSource` (the path overload delegates to it). Tests: the source
  vanishes before / at / after chunk boundaries -> `IoError`, exactly one failed read, **no output file and no stray
  file in the directory**; the same destination name is reusable and the retry round-trips; a source that claims more
  bytes than the file holds fails the same way; after a failed stream the file handle is released (Windows refuses
  to delete a file with an open handle, so a successful `remove` is the proof); closing the OS handle behind a
  `FileSource` gives a structured `IoError` ("Win32 error 6") from `read_chunk` / `read_range`.
* Finding recorded as a test: the streaming pipeline opens its **own** handle on the path, so a yanked random-access
  handle does not affect streaming (asserted). The first version of that test assumed otherwise and failed; the
  assumption, not the product, was wrong.
* Not run: pulling the cable of the real USB HDD during a model load.

## (c) Shutdown
* Control plane + HTTP server with the fake engine (`test_http_shutdown.cpp`): idle (twice, idempotent), loaded model
  with a keep-alive client, load in flight, queued job behind a load, unload racing a load, streaming generation,
  unload during generation. Real model (`tests/integration/shutdown_real`, `HCS_TEST_GGUF` = LFM3-Omni-2B Q8_0, CPU
  engine, real socket): idle with a resident model, shutdown during a real SSE generation (cancelled in ~1.6 s with
  the client stream ended), shutdown right after a load request. The first test absorbed a 30 s cold file load.
* A model load is a blocking engine call and cannot be interrupted: `graceful_shutdown` waits for it (bounded by the
  load time). This is asserted (`ShutdownDuringAnUninterruptibleLoad...`), not hidden.

## (d) Supply chain
* `npm audit` (npm 11.21.1): `web/` has no dependencies -> 0; the pinned runtime package
  `@huggingface/transformers@4.3.2` with its transitive tree (incl. onnxruntime-web) -> 0 vulnerabilities.
* `cargo audit` 0.23.0 (installed with `cargo install cargo-audit --locked` from crates.io; RustSec DB of 1550
  advisories) over `external/prime-agent/Cargo.lock` (356 crates): 4 vulnerabilities + 3 warnings. Reachability
  analysed in `docs/licence-audit.md` Findings 5: the three quinn advisories are in the lock file only
  (`cargo tree --all-features --target all` has no `quinn*` node); `rsa` (Marvin) is triaged upstream in
  prime-agent's own `deny.toml`.
* Licences of the 345 registry crates + 11 workspace members (incl. vendored `crossterm`, MIT) against prime-agent's
  `deny.toml` allowlist: 0 mismatches. This is a `cargo metadata` reproduction, **not** the literal `cargo deny` run.
* OSV.dev query of every pinned commit: **cpp-httplib v0.37.2 -> 9 CVEs, 4 in the server path -> bumped to v0.44.0**
  as its own compatibility change; CPU Release suite re-run after the bump: 814 registered, 0 failed. OSV still lists
  4 client/TLS/WebSocket advisories on v0.44.0 (unreachable here). Vendored stb/libwebp/oniguruma/utf8proc: not
  queried (NOT RUN). The Debug and Vulkan presets were not re-run against v0.44.0 (NOT RUN); the static preset was
  built fresh with it (814 registered, 0 failed).
* `licence-reverify.txt`: licence files and submodule pins re-checked against `docs/licence-audit.md`; all
  consistent. The nested sd.cpp submodules (ggml, libwebp) are not initialised in this worktree, so their licence
  files were read from the main checkout.

## (e) Restricted environment (partial clean-machine proxy)
`scripts/restricted-env-check.ps1` copies `HCSInfer.exe` ALONE into a fresh temp dir and runs it with PATH cleared and
a minimal environment (SystemRoot, TEMP, HCS_STATE_DIR only), no Visual Studio, no Vulkan SDK, repository not on the
path. Build under test: `windows-msvc-release-static`, sha256
`42067d40e8fb3142a7e3a564a61d7c13ffe6f88bf1868cb55cd50ee8bfd127af` (44 208 128 bytes), built fresh with v0.44.0.
* `dumpbin /dependents`: `ADVAPI32.dll`, `WS2_32.dll`, `KERNEL32.dll` only.
* `--help` exit 0, `doctor --json` exit 0 and valid JSON, a **real CPU generation** with LFM3-Omni-2B Q8_0 exit 0
  ("static ok", load 17.4 s, 9.6 decode t/s), `serve` answered `GET /hcs/v1/health` 200; the live process had 14
  modules loaded, **all under `C:\Windows`** (system DLLs incl. `ucrtbase.dll`, an OS component), none from elsewhere.
* Full CTest of the static preset: 814 registered, 778 passed, 36 skipped (NOT RUN), 0 failed.
* **What this is not:** the host still has the VC++ redistributable, a Vulkan driver, Visual Studio and the Vulkan SDK
  installed. It proves no sidecar file and no PATH entry is needed, not that a bare Windows install runs the EXE.
  `clean-machine-availability.txt`: Windows Sandbox is not enabled, Hyper-V needs elevation, so no clean VM exists
  without changing system features (not done). The real clean-machine test and a `/MT` Vulkan build stay **NOT RUN**.

## Other changes in this wave
* `tests/e2e/gui_real/e2e_gui.py` probes each headless browser with a trivial page and uses the first working one. The
  first full run of the suite (`ctest-release-cpu-first-run-gui-edge-failure.txt`) failed `e2e_gui_real` Phase 2
  because the installed `msedge.exe` exits silently with no output on this machine (even for a `data:` URL, outside
  HCSInfer), so the test never reached the working Chrome. Environment drift plus a harness that trusted the first
  installed browser, not a product defect; the re-run is green (Chrome).

## Reproduce
```powershell
cmake --preset windows-msvc-release; cmake --build --preset windows-msvc-release --parallel
ctest --test-dir build\windows-msvc-release --output-on-failure -j4
$env:HCS_TEST_GGUF = 'D:\HCSModels\llm\LFM3-Omni-2B\LFM3-Omni-2B-Q8_0.gguf'
build\windows-msvc-release\bin\hcs_test_shutdown_real.exe
.\scripts\restricted-env-check.ps1 -Exe build\windows-msvc-release-static\bin\HCSInfer.exe -Model $env:HCS_TEST_GGUF -OutJson restricted.json
```
