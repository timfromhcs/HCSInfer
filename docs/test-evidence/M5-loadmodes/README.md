# M5 Stage A: llama.cpp weight-load modes on the real model (benchmark matrix)

Date: 2026-11-17. Machine: Windows 11 26200, Ryzen 7 7735HS (8C/16T), 21.2 GB RAM, Radeon 680M, MSVC 19.44.35228.
Build: `build/img-api` (Release, CPU-only). Model: `qwen3-0.6b-q4_k_m.gguf`, 484,219,712 bytes, SHA-256
`4ea794120a3b2244a6e8a36354f1aa53e7f0c52b2f1f280dde0d7f522f37ae7c`, read from `E:` (USB, NTFS).

AGENTS.md 11.2 Stage A asks for exactly this before any HCS-owned loading pipeline: measure what the pinned upstream
supports (`mmap`, `mlock`, `mmap+mlock`, no-mmap, Direct I/O) instead of inventing a format first.

```
HCSInfer benchmark --suite quick --model <gguf> --backend cpu --load-mode <m> --no-hash --export <json>
# m in: auto mmap none mlock mmap+mlock direct_io
```

Identical parameters in every run: context 4096, max-new-tokens 64, greedy (temperature 0), seed 42, warm-up 1,
3 measured iterations, 199-token fixed prompt. `--no-hash` was used so no run re-reads the whole file for SHA-256
(the hash is recorded in the runs above and in `../benchmark/`).

Each run is `loadmode-<m>-cpu-qwen3-0.6b-q4_k_m.json` (+ `.stdout.json`, + `.stderr.log`) in this directory.
All six runs exited 0 and produced the AGENTS.md 21.1 document with `"load_mode": "<m>"`.

## Measured

| mode | cold load ms | warm load median ms | warm TTFT ms | warm prompt t/s | warm decode t/s | peak working set MB | peak commit MB |
|---|---|---|---|---|---|---|---|
| `auto` (engine default) | 630 | 664 | 498 | 411 | 60.8 | 1106 | 1096 |
| `mmap` | 674 | 651 | 535 | 381 | 60.2 | 1106 | 1096 |
| `none` (read into RAM) | 768 | 655 | 519 | 394 | 59.7 | 984 | 1347 |
| `mlock` | 759 | 836 | 528 | 387 | 59.8 | 984 | 1347 |
| `mmap+mlock` | 900 | 871 | 508 | 402 | 59.3 | 1170 | 1097 |
| `direct_io` | **9538** | **9092** | 530 | 386 | 58.8 | 985 | 1348 |

## What this actually shows

1. **All six modes are accepted and correct on this build/machine.** Nothing crashed, nothing fell back silently
   (an unknown mode is `InvalidArgument`, exit code 2 - covered by `tests/contract/cli`).
2. **`direct_io` really bypasses the OS file cache and costs ~14x the load time here**: 9.5 s vs 0.63 s cold. The
   magnitude matches the independently measured raw HDD throughput of this drive (65 MiB/s in
   `../M0/readbench-usb-hdd.json`): 484 MB / 65 MiB/s ~ 7.8 s, plus GGUF parsing. This is the one effect in the
   table that is large enough to survive the uncontrolled cache state, because it is the only mode that ignores the
   cache by design.
3. **`auto` == `mmap`** in both time and memory, which confirms that llama.cpp's default on Windows is mmap.
4. **Memory residency differs as expected**: mmap-backed weights (`auto`, `mmap`) commit ~1.10 GB, while
   read-into-RAM modes (`none`, `mlock`, `direct_io`) commit ~1.35 GB and have a ~120 MB *lower* working set;
   `mmap+mlock` is highest (1.17 GB working set) because the pages are both mapped and locked.
5. **No throughput difference is measurable** (58.8-60.8 t/s decode, all inside run-to-run noise): the 0.48 GB model
   fits comfortably in this machine's RAM, so once loaded, the load mode cannot matter.

## What this does NOT show (honest limits)

* **The Windows file cache was not controlled** (flushing it needs administrator rights). The runs happened in the
  order listed, on a machine where the same file had already been read several times that day, so the load-time
  column is *not* a cold-cache comparison - except for `direct_io`, which ignores the cache by construction. No
  "mode X loads faster" claim is made beyond that one effect.
* No model larger than RAM was available, so the storage-bound case (weights that cannot stay resident, i.e. where a
  load mode would really decide throughput) is **NOT RUN**.
* CPU only: Vulkan weight placement is a separate question (`mp.n_gpu_layers`/device), not a load mode.
* One model, one drive, one machine, 3 measured iterations. `mlock` succeeded without administrator rights here;
  other configurations may hit the Windows working-set limit and fail (that would be a clean load error).
* The banner "cold" in each JSON means *process-cold* (`file_cache: not_controlled`), as documented in
  `../benchmark/README.md`.

## Consequence for the M5 plan

* No compressed/owned format is justified by this data for a model that fits in RAM: upstream loading is already
  fast here (~0.65 s for 484 MB) and the HCSMODEL container would have to beat that.
* `direct_io` is a *diagnostic* mode (raw storage speed) rather than a speed-up, and should stay opt-in. The default
  stays `auto` (mmap), which also has the lowest commit.
* The next M5 step that can still produce a real gain is the one AGENTS.md 11.3 describes for machines with less
  RAM than the model: a bounded asynchronous read pipeline feeding the engine, measured on a model that does not fit
  in RAM. That comparison is **BLOCKED** here: no such model is available locally, and the file cache cannot be
  flushed without administrator rights. It needs either a >21 GB model or an administrator-assisted cache flush in a
  controlled test.
