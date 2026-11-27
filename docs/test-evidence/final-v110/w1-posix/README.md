# w1-posix: Linux CPU build evidence (2026-11-27)

Environment: WSL2 Ubuntu (kernel 6.18 WSL2), g++ 15.2.0, clang++ 21.1.8, ninja, cmake; AMD Ryzen 7 7735HS
(WSL exposes 3 cores / 6 threads). Build trees under `D:\HCSScratch\linux-w1posix` (not on C:). Commit under test:
81ac876 plus the follow-up in this branch (clang `-Wstring-concatenation` fix in `tests/unit/agent/test_event_parser.cpp`,
docs).

| Gate | Result | Evidence |
|---|---|---|
| gcc 15 Release, `-Werror` ON | PASS | build completes; `gcc-ctest-portable.txt` |
| clang 21 Release, `-Werror` ON | PASS (after fixing one real clang-21-only warning in a test file) | `clang-ctest-portable.txt` |
| Portable ctest set (`unit\|contract\|parser\|kernel-cpu`), 963 tests, both compilers | 962 passed, 1 FAILED, 9 skipped | the two `*-ctest-portable.txt` files |
| Failure: `CliRemote.NoServerRunningIsAClearIoErrorNotAStub` | environment: WSL localhost relay (a Windows `dllhost` listener) accepts then resets a just-closed port, so the client reports Timeout (14) instead of IoError (8). A never-bound port gives exit 8 with the expected message. | not a code defect; NOT a pass either |
| `FsUtil.ConcurrentWritersNeverProduceMixedContent` | passes 25/25 on tmpfs, ~20% failures on drvfs `/mnt/d` (9p); the portable run above used `TMPDIR=/dev/shm` | environment (filesystem), recorded in `docs/build.md` |
| Skips | MemoryProbe GPU heap (no real GPU in WSL), 3 ORT tests (no ORT on Linux), 5 ApiSocketAudit raw-socket tests (WINDOWS-socket specific) | listed in the ctest logs |
| Real CPU generation on Linux (LFM3-Omni-2B Q8_0 from `/mnt/d`, read-only) | PASS: reply "HCSInfer smoke test passed.", 9.87 tok/s decode, TTFT 599 ms, 16.5 s cold load via `pread` | `chat-cpu.json`, `model-sha-prefix.txt` |
| `serve` on Linux: `/health` and runtime | PASS | `serve-health.json`, `serve-runtime.json` |
| `doctor` on Linux (+ Vulkan loader seam against Mesa llvmpipe, and with Vulkan disabled) | PASS (unknowns reported as `unknown`) | `doctor.txt`, `doctor-no-vulkan.json` |
| Binary links only libm/libc dynamically (libstdc++/libgcc static) | PASS | `ldd.txt`, `file.txt` |
| Linux Vulkan GGML build / real AMD GPU on Linux | NOT RUN | no `glslc` in the WSL image; nothing installed (WSL vhdx is on C:) |
| macOS arm64 | NOT RUN | cannot be built locally |
| Coding-agent sidecar on POSIX | Planned (returns `Unsupported`) | `docs/build.md` |
| MSVC regression (`windows-msvc-release`, MSVC 19.44) | PASS: build OK; `unit|contract|parser|kernel-cpu` 1003/1003 passed (Windows runs 40 more tests than Linux) | `msvc-ctest-portable.txt`, `msvc-build-tail.txt` |
