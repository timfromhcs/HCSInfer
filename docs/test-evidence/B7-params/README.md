# B7: stable-diffusion.cpp compute backend vs parameter backend

Date: 2026-11-17. Machine: Windows 11 26200, Ryzen 7 7735HS (8C/16T), 21.2 GB RAM, Radeon 680M (Vulkan0, AMD
proprietary driver). Upstream CLI: `build/sd-vulkan/bin/sd-cli.exe` (stable-diffusion.cpp `master-1106-532d2c1`,
pinned commit `532d2c15c430a785a712b2d94281f04e733008a4`, patched GGML build - see `../M1/sd/`).
Model: `D:\HCSModels\diffusion\sd15-q8_0.gguf` (1,763,578,176 bytes, SD1.5 Q8_0).

AGENTS.md 11.2 asks to test the compute backend separately from the parameter backend for diffusion, and to treat
parameter placement on disk as a capacity fallback - not as an assumed speed-up or slow-down. This is that
measurement, at the smallest useful size (256x256, 2 steps, seed 42, default cfg 7, one fixed prompt).

```
sd-cli.exe -m <model> -p "<prompt>" -W 256 -H 256 --steps 2 -s 42 -o <out.png> --backend <b> --params-backend <p>
```

## Measured (one sample each; `b7-summary.json` has the machine-readable numbers)

| run | compute backend | params backend | wall ms | `generate_image` s (CLI's own) | peak working set MB | peak commit MB | output sha256 (first 8) |
|---|---|---|---|---|---|---|---|
| a | `vulkan0` | `cpu` | 5218 | 3.07 | **1772.8** | **3572.3** | `86560b78` |
| b | `vulkan0` | `disk` | **3288** | **2.66** | **624.8** | **2255.0** | `0efaefb3` |
| c | `cpu` | `cpu` | 9676 | 9.20 | 2219.5 | 2410.0 | `dfd02a6f` |

Per-stage times from the CLI's own log lines (`.log` files in this directory):

| stage | a (vulkan0/cpu) | b (vulkan0/disk) | c (cpu/cpu) |
|---|---|---|---|
| text encoder (`get_learned_condition`) | 0.42 s | 0.28 s | 0.40 s |
| sampling (2 steps) | 1.46 s | 1.34 s | 4.37 s |
| VAE decode (`decode_first_stage`) | 1.09 s | 1.03 s | 4.42 s |

## What this shows, and what it does not

1. **The `disk` parameter backend used ~1.1 GB less process memory and was faster here** (3.29 s vs 5.22 s wall, peak
   working set 625 MB vs 1773 MB). The CLI's own accounting explains it: with `--params-backend cpu` it reports
   `total params memory size = 1683.69MB (VRAM 0.00MB, RAM 1683.69MB)`, i.e. all 1.68 GB are staged in process RAM and
   copied to the GPU per stage; with `disk` it reports the same total as `(VRAM 1683.69MB, RAM 0.00MB)` and only one
   stage shows a `copy_to_backend` cost (0.23 s). Nothing is written to the SSD by HCSInfer here - this is sd.cpp's own
   file-backed placement.
2. **This does NOT mean "disk parameters are faster".** The model file sits on `D:`, which the M1 evidence describes as
   internal NVMe-class storage, and its content was in the Windows file cache after earlier runs. On the external USB
   HDD (measured at 65 MiB/s raw) streaming 1.68 GB per pass would cost ~26 s, so the ranking would very likely invert.
   A USB-HDD `disk`-params measurement is **NOT RUN**.
3. **Compute backend matters much more than parameter placement at this size**: CPU compute is ~3x slower on sampling
   (4.37 s vs 1.46 s) and ~4x on VAE decode (4.42 s vs 1.09 s) than Vulkan0.
4. **The output differs between placements a and b** (different PNG hashes) even though the compute backend and seed are
   identical. Most likely because the parameter backend changes where dequantization happens, so the floating-point
   path is not bit-identical. **Whether the two images are visually equivalent is NOT verified** (no SSIM/pixel-diff
   check and no human comparison in this run); the honest statement is "not byte-identical, equivalence unassessed".
5. Limits of this evidence: **one sample per configuration** (no repetitions, so the wall times are single samples, not
   benchmark medians), one model, one size, 2 steps, one machine; the file cache was not controlled; run `c` ran last
   with the GPU idle and the other runs' memory freed, so process-peak comparisons between a and c are indicative only.
   Peak memory was sampled by polling the process every 200 ms, so a very short spike could be missed.
