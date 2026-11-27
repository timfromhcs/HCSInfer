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
| use it from Python | `pip install hcsinfer-1.1.0rc1-py3-none-<plat>.whl` (`win_amd64`, `win_arm64`, `manylinux_2_28_x86_64`/`aarch64`, `macosx_11_0_arm64`, `macosx_10_15_x86_64`; platforms without a native wheel get the pure `py3-none-any` wheel) | Python >= 3.9 |
| Python code only (bring your own binary) | `hcsinfer-1.1.0rc1-py3-none-any.whl` or `hcsinfer-1.1.0rc1.tar.gz` | `HCSINFER_EXE=<path to HCSInfer>` |
| the browser demo (WebGPU/WASM) | `hcs-studio-web-dist.zip` | any static web host; not deployed by this project yet |

Check the download first:

```powershell
Get-FileHash -Algorithm SHA256 HCSInfer.exe     # compare with the line in SHA256SUMS.txt
```

The binaries are **unsigned**; Windows SmartScreen may show a warning. Code signing is NOT RUN (no certificate).

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
* Only one GPU (Radeon 680M) and one OS build were tested. Other GPUs are NOT RUN; Linux has a CPU-only build proven in WSL2 (no Linux GPU run, no Linux package).
* PyPI, GitHub Releases and Vercel: nothing has been uploaded or deployed.
