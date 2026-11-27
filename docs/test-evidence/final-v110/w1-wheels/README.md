# w1-wheels evidence (2026-11-27)

Machine: Windows 11, Python 3.14.6, hatchling 1.33.0, mypy 2.4.1, build 1.6.1, twine 7.0.0, actionlint 1.7.12.

| Check | Result |
|---|---|
| `pytest` in `python/` (new `tests/test_wheel_tags_and_exe.py`: tag allow-list, host derivation, hook with fake binaries, discovery per simulated OS, Vulkan-loader probes) | PASS: 43 passed, 5 skipped (real-exe tests need `HCSINFER_EXE`; one POSIX exec-bit test skipped on Windows) |
| `mypy --strict` (src + tests) | PASS: no issues in 9 files |
| `scripts/build-python.ps1` with the real static CPU + Vulkan release EXEs from `build/windows-msvc-release-static*` | PASS: `hcsinfer-1.1.0rc1-py3-none-win_amd64.whl` (50.7 MB), WHEEL tag `py3-none-win_amd64`, Root-Is-Purelib false, both EXEs inside, `twine check --strict` PASSED |
| Clean venv `pip install --no-index --no-deps`, `hcsinfer --version` | PASS: `HCSInfer 1.1.0-rc.1`, rc 0; `HCSINFER_BACKEND=cpu` resolves `HCSInfer.exe`, auto resolves `HCSInfer-vulkan.exe` (vulkan-1.dll present) |
| No binary staged: `python -m build` | PASS: `py3-none-any` wheel + sdist, `twine check --strict` PASSED |
| `actionlint` on `wheels.yml`, `pypi.yml` | PASS (rc 0); action pins verified with `git ls-remote` |
| Hosted run of wheels.yml (Linux, macOS, win-arm64 jobs, manylinux glibc check, auditwheel show) | NOT RUN (cannot execute locally) |
| TestPyPI / PyPI upload | NOT RUN (owner setup required, docs/pypi-publishing.md) |
