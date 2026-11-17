# M2 evidence — diffusion (image) engine adapter and `HCSInfer image`

Status: **PASS** for text-to-image and img2img through the HCS-owned adapter on both CPU and
Vulkan. Vision/VLM remains BLOCKED (no licensed VLM model available) — see `../STATUS.md`.

## Environment

| Item | Value |
|---|---|
| Machine | AMD Ryzen 7 7735HS (8C/16T, Zen 3+) + Radeon 680M (RDNA 2, 12 CU) |
| OS | Windows 11 x64 |
| Compiler | MSVC 19.44.35207 (VS 2022 17.14) |
| CMake / Generator | CMake 4.4.0 / Ninja 1.13.2 |
| Vulkan SDK (build only) | `C:\VulkanSDK\1.4.357.0` |
| Driver | AMD proprietary (from `ggml_vulkan` device line below) |
| Model | `D:\HCSModels\diffusion\sd15-q8_0.gguf` |
| Model size / sha256 | 1,763,578,176 B / `d0555243938c62faeefb4ac93f6c7a053ad373a4290c5256bce229aeb193bf94` |
| Model licence | **unverified** — internal test only, never redistributed |
| Engines | llama.cpp `d3c3e242`, stable-diffusion.cpp `532d2c15` via `SD_USE_UPSTREAM_GGML=ON` |

Builds: `scripts\build.ps1 -Preset windows-msvc-release` and
`scripts\build.ps1 -Preset windows-msvc-release-vulkan` (both `BUILD OK`).

## Runs

All commands were run from `D:\HCSInfer`. `--json` writes the machine-readable result to stdout;
stderr is reserved for diagnostics.

### CPU text-to-image (Release, no Vulkan)

```
HCSInfer.exe image generate --model D:\HCSModels\diffusion\sd15-q8_0.gguf ^
  --prompt "a red apple on a plain table" --width 256 --height 256 --steps 2 --backend cpu --json
```

- Result: `cpu-generate-256x256-2step.stdout.json`, image `cpu-generate-256x256-2step.png` (115,863 B)
- Exit code 0, stderr empty (0 B)
- `backend=cpu`, device `AMD Ryzen 7 7735HS with Radeon Graphics (CPU)`
- `load_ms=145.18`, `generation_ms=9048.22`, `total_ms=9050.28`

### CPU img2img / edit (same build)

```
HCSInfer.exe image edit --model D:\HCSModels\diffusion\sd15-q8_0.gguf ^
  --prompt "a red apple on a plain table, photo" --input artifacts\image-smoke\cpu-256x256-2step.png ^
  --width 256 --height 256 --steps 2 --strength 0.6 --backend cpu --json
```

- Result: `cpu-edit-256x256-2step.stdout.json`, image `cpu-edit-256x256-2step.png` (132,762 B)
- Exit code 0, stderr empty (0 B)
- `mode=edit`, `steps=2`, `generation_ms=17939.00`

### Vulkan text-to-image (Release + Vulkan)

```
HCSInfer.exe image generate --model D:\HCSModels\diffusion\sd15-q8_0.gguf ^
  --prompt "a red apple on a plain wooden table, photo" --width 512 --height 512 ^
  --steps 4 --backend vulkan --json
```

- Result: `vulkan-512x512-4step.stdout.json`, image `vulkan-512x512-4step.png` (657,643 B)
- Exit code 0 (`vulkan-512x512-4step.exit.txt`)
- `backend=vulkan`, device `AMD Radeon(TM) Graphics (Vulkan0)`
- `load_ms=288.68`, `generation_ms=11992.25` (warm file cache)
- stderr contains only the ggml device banner (kept intentionally as device evidence):

```
ggml_vulkan: Found 1 Vulkan devices:
ggml_vulkan: 0 = AMD Radeon(TM) Graphics (AMD proprietary driver) | uma: 1 | rebar: 0 |
  fp16: 1 | bf16: 0 | fp4: 0 | warp size: 32 | shared memory: 32768 | int dot: 1 | matrix cores: none
```

## Cold vs warm (important)

An earlier cold run of the same 512x512 model, 15 steps, reported `generation_ms=217539.23`
(≈218 s), of which step 1 alone took ≈182 s while stable-diffusion.cpp streamed 686 tensors from
the model file at an effective 5–13 MB/s. The later run above (Windows file cache warm) needed
≈12 s for 4 steps. **Cold and warm numbers must not be compared as if they were the same
configuration**; `load_ms` only covers `sd_ctx_create`, because tensor materialisation is lazy and
happens on the first sampling step. Model-read throughput on this machine is dominated by the
`ggml` per-tensor read path, not by HCS I/O — the Model Vault work (M5) is what will address it.

## Fix verified in this evidence set

stable-diffusion.cpp draws its own progress bars on **stdout**
(`src/core/util.cpp: print_progress_line`) unless a legacy progress callback is registered. That
corrupted `image --json` and would corrupt the MCP stdio JSON-RPC stream. The adapter now installs
a no-op `sd_set_progress_callback` in `ensure_log_capture()`; when `HCS_DIFFUSION_VERBOSE` is set it
writes one line per step to stderr instead. All files above were produced after that fix and are
byte-verified to contain only JSON on stdout.

## Covered by automated tests

`tests/contract/cli/test_cli.cpp` covers the `image` command contract (argument validation, exit
codes, JSON schema shape, unsupported load modes, and non-silent backend failures) with no model
required. Real-model results above are the manual/integration evidence.
