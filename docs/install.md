# Install HCSInfer

HCSInfer 1.1.0-rc.1 is a release candidate verified on one machine (Ryzen 7 7735HS, Radeon 680M, Windows 11).
Nothing here is published yet: the files below are produced locally by `scripts/release-local.ps1` or by the draft
release workflow (`.github/workflows/release.yml`). Models are not included; bring your own GGUF files and respect their
licences. Details of every artefact: [packaging.md](packaging.md).

## Which file do I need?

| I want | Use | Needs |
|---|---|---|
| one file, runs anywhere on Windows x64 (CPU) | `HCSInfer.exe` | nothing else (no Visual C++ Redistributable) |
| the same plus GPU acceleration (Vulkan) | `HCSInfer-vulkan.exe` | the Vulkan loader `vulkan-1.dll` that your GPU driver installs; it does not start without it |
| everything in one folder with licences, SBOM, checksums | `HCSInfer-1.1.0-rc.1-windows-msvc-release-static-portable.zip` | unzip |
| Linux x86_64 (CPU) | `HCSInfer-<ver>-linux-x86_64.tar.gz` (`HCSInfer`, and `HCSInfer-vulkan` when it was built) | glibc >= 2.28 (RHEL/Alma 8, Debian 10 and newer); libstdc++ is linked statically |
| Linux aarch64 (CPU), **experimental** | `HCSInfer-<ver>-linux-aarch64.tar.gz` | glibc >= 2.28 |
| macOS arm64 (CPU, no Metal), **experimental** | `HCSInfer-<ver>-macos-arm64.tar.gz` | macOS 11 or newer, Apple silicon |
| use it from Python | `pip install hcsinfer-1.1.0rc1-py3-none-<plat>.whl` (`win_amd64`, `win_arm64`, `manylinux_2_28_x86_64`/`aarch64`, `macosx_11_0_arm64`, `macosx_10_15_x86_64`; platforms without a native wheel get the pure `py3-none-any` wheel) | Python >= 3.9 |
| Python code only (bring your own binary) | `hcsinfer-1.1.0rc1-py3-none-any.whl` or `hcsinfer-1.1.0rc1.tar.gz` | `HCSINFER_EXE=<path to HCSInfer>` |
| the browser demo (WebGPU/WASM) | `hcs-studio-web-dist.zip` | any static web host; not deployed by this project yet |

Check the download first:

```powershell
Get-FileHash -Algorithm SHA256 HCSInfer.exe     # compare with the line in SHA256SUMS.txt
```

The binaries are **unsigned**; Windows SmartScreen may show a warning. Code signing is NOT RUN (no certificate).

## Linux and macOS (tar.gz)

Support levels are in [support-matrix.md](support-matrix.md): Linux x86_64 CPU is the supported POSIX build, Linux aarch64,
macOS arm64 and every Vulkan-on-Linux use are **experimental**. Nothing here has been run on a real Linux GPU or on macOS
by the project.

```bash
sha256sum -c SHA256SUMS.txt --ignore-missing          # macOS: shasum -a 256 -c SHA256SUMS.txt --ignore-missing
tar -xzf HCSInfer-<ver>-linux-x86_64.tar.gz && cd HCSInfer-<ver>-linux-x86_64
./HCSInfer --help
./HCSInfer doctor --json                                  # unknown values stay "unknown"
./HCSInfer models scan ~/models                           # read-only
./HCSInfer chat --model <id-or-path> --prompt "Hello" --max-tokens 64
./HCSInfer serve --model-root ~/models                    # API + web UI at http://127.0.0.1:8765 (token on stderr)
```

* `HCSInfer-vulkan` (Linux x86_64 only, present only when that build succeeded) needs the system `libvulkan.so.1` plus a
  driver (Mesa RADV or a vendor ICD) and is started with `--backend vulkan`. It does not start without the loader.
* The CPU binary needs only libc/libm (and libpthread/libdl/librt on older glibc); the release gate fails the build if
  anything else, an RPATH, or a GLIBC symbol newer than 2.28 appears.
* macOS may refuse to open the unsigned binary (Gatekeeper). Remove the quarantine flag yourself if you trust the download:
  `xattr -d com.apple.quarantine ./HCSInfer`. The project does not sign or notarize.
* The coding-agent sidecar is Windows only; on Linux/macOS the agent features report `Unsupported`.

## Run the static EXEs

Two static EXEs: `HCSInfer.exe` imports Windows system DLLs only; `HCSInfer-vulkan.exe` also imports the system
`vulkan-1.dll`. They are not called "single-file" until a clean-machine run exists (NOT RUN).

```powershell
.\HCSInfer.exe --help
.\HCSInfer.exe doctor --json                              # hardware report; unknown values stay "unknown"
.\HCSInfer.exe models scan D:\Models                      # read-only scan, registers GGUF files
.\HCSInfer.exe chat --model <id-or-path> --prompt "Hello" --max-tokens 64
.\HCSInfer.exe serve --model-root D:\Models               # API + web UI at http://127.0.0.1:8765 (token on stderr)
```

Use `HCSInfer-vulkan.exe ... --backend vulkan` for the GPU. An explicit `--backend vulkan` fails loudly instead of
silently falling back to the CPU.

## Python

```powershell
python -m venv venv; .\venv\Scripts\Activate.ps1
pip install .\hcsinfer-1.1.0rc1-py3-none-win_amd64.whl
hcsinfer --help
```

```python
import hcsinfer
with hcsinfer.Server(["D:/Models"]) as srv:           # scans the folder, serves on loopback with a random token
    c = srv.client()
    print(c.chat_text("Say hi", model="<id>", max_tokens=32))
```

`HCSINFER_BACKEND=cpu|vulkan|auto` chooses the bundled executable (auto = Vulkan only when the system Vulkan loader exists: `vulkan-1.dll` on Windows, `libvulkan.so.1` on Linux);
`HCSINFER_EXE` points at any other executable. The wheel has no Python dependencies. See `python/README.md`.

## Build from source

See [build.md](build.md) (presets `windows-msvc-release`, `...-static`, `...-static-vulkan`) and [packaging.md](packaging.md).

## What is not proven

* A run on a clean Windows machine (no redistributable, no SDK) is NOT RUN: Windows Sandbox is not installed here and
  enabling it needs elevation. A restricted-environment run (empty directory, empty PATH, only system DLLs loaded) PASSED.
* Only one GPU (Radeon 680M) and one Windows build were tested on real hardware. Other GPUs are NOT RUN. Linux x86_64 has a CPU
  build proven in WSL2 plus a release-pipeline build whose hosted run is NOT RUN until it has executed; Linux Vulkan,
  Linux aarch64 and macOS arm64 are experimental and have no real-hardware run.
* PyPI, GitHub Releases and Vercel: nothing has been uploaded or deployed.
