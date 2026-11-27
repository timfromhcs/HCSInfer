# hcsinfer (Python)

Python launcher and client for [HCSInfer](https://github.com/timfromhcs/HCSInfer), a local-first AI runtime for text
chat, vision and image generation (llama.cpp / stable-diffusion.cpp behind one control plane).

- A **platform wheel** (`py3-none-<plat>`: `win_amd64`, `win_arm64`, `manylinux_2_28_x86_64`/`aarch64`, `macosx_11_0_arm64`,
  `macosx_10_15_x86_64`) bundles the CPU `HCSInfer[.exe]` and, when present, `HCSInfer-vulkan[.exe]` (CPU + Vulkan; chosen only
  when the system Vulkan loader exists: `vulkan-1.dll` / `libvulkan.so.1`). Only the Windows x64 wheel has been verified so far.
- The **sdist / pure wheel** (`py3-none-any`) contains only the Python code (fallback for platforms without a native binary);
  point it at an executable with `HCSINFER_EXE`.
- No Python dependencies; the client uses only the standard library.
- Models are **not** included. Use your own GGUF files and respect their licences.

```powershell
pip install hcsinfer
hcsinfer --help
hcsinfer doctor --json
```

```python
import hcsinfer

print(hcsinfer.doctor()["schema_version"])            # hardware report (unknown values stay "unknown")

with hcsinfer.Server(["D:/Models"]) as srv:            # scans the folder, serves on 127.0.0.1 with a random token
    c = srv.client()
    print(c.models())
    print(c.chat_text("Say hi", model="my-model-id", max_tokens=32))
    for piece in c.chat_stream("Tell a short joke", model="my-model-id"):
        print(piece, end="")
```

`Client(base_url, token)` also talks to a server you started yourself (`HCSInfer serve`). Errors raise
`hcsinfer.HcsError` (`status`, `code`, `request_id`).

Environment: `HCSINFER_EXE` (explicit executable), `HCSINFER_BACKEND` (`auto` | `cpu` | `vulkan`).

Licence: MIT for this package. HCSInfer bundles third-party components; see `THIRD_PARTY_NOTICES.md` in the
repository.
