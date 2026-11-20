# hcsinfer (Python)

Python launcher and client for [HCSInfer](https://github.com/timfromhcs/HCSInfer), a local-first AI runtime for text
chat, vision and image generation on Windows (llama.cpp / stable-diffusion.cpp behind one control plane).

- The **platform wheel** (`py3-none-win_amd64`) bundles the single-file `HCSInfer.exe` (CPU, static CRT) and, when
  present, `HCSInfer-vulkan.exe` (CPU + Vulkan; needs the system `vulkan-1.dll` from your GPU driver).
- The **sdist / pure wheel** contains only the Python code; point it at an executable with `HCSINFER_EXE`.
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
