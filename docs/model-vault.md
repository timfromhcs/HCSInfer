# Model Vault (`hcs_storage`)

Status: **experimental** (M5/M6 groundwork). The Model Vault is the storage layer between a model file on the
external USB drive and the engines. It validates model files, streams them into RAM through a bounded pipeline and
offers an optional chunked container (HCSMODEL). Decisions: ADR-005 (policy) and ADR-006 (container).

## What it guarantees, and what it does not

Guaranteed by the code (and unit-tested):

- Model files are opened **read-only**. The vault never modifies, moves or deletes a model.
- Streaming memory is bounded: `buffers * (max stored chunk + max raw chunk)`, allocated once, optional hard budget
  (`StreamOptions::max_buffer_bytes`, fails with `OutOfMemory` before allocating).
- There is **no HCS-owned decompressed model cache or staging file on the SSD**. Decoded data lives only in the
  pipeline slots and the RAM the caller's sink owns. The only thing that ever writes a model-sized file is the
  explicit, opt-in `write_hcsmodel` / `hcs_storage_bench`, into a directory the caller names; it creates a new file,
  refuses to overwrite, and removes only its own partial file.
- All input is untrusted: GGUF and HCSMODEL headers are validated (caps, checked arithmetic, bounds against the real
  file size) before any allocation or read that depends on them.

Not promised:

- Windows may still page process memory to the system page file. HCSInfer never disables, resizes or bypasses the
  page file.
- A USB HDD is not RAM or VRAM. Re-reading weights from it during token generation would be extremely slow; the
  vault loads sequentially into RAM, it does not make a disk-resident working set fast.
- Lossless compression does **not** reliably shrink quantized GGUF weights (see measurements below), and a
  compressed container is only faster when the drive is slower than decode + hash.

## Components

| Piece | Header | Purpose |
|---|---|---|
| `Sha256` | `hcs/storage/sha256.hpp` | FIPS 180-4; SHA-NI path auto-selected, portable fallback |
| codecs | `hcs/storage/codec.hpp` | Raw, LZ4 v1.10.0, Zstandard v1.5.7 (static, bounded decoders) |
| `FileSource` / `MemorySource` | `file_source.hpp` | read-only random access; Win32 overlapped handle |
| GGUF parser | `gguf.hpp` | safe header/metadata/tensor-table validation (v2, v3) |
| HCSMODEL parser/writer | `hcsmodel.hpp`, `writer.hpp` | container format, see below |
| `stream_chunks` | `stream.hpp` | IOCP read -> N slots -> decode workers -> in-order sink |
| `IModelReader` | `model_reader.hpp` | `PlainFileReader` (GGUF/any file) and `HcsModelReader` behind one interface |

### Streaming pipeline

```text
USB drive --overlapped ReadFile (IOCP, <= N in flight)--> slot buffers (N)
   --> decode + SHA-256 workers (std::thread) --> sink(chunk i) strictly in order --> caller's RAM
```

- One I/O thread issues reads for chunk `j` only when slot `(j - begin) % N` is free and reaps completions from the
  completion port; the consumer (calling thread) frees a slot after the sink returned and posts a wake-up packet.
- Cancellation (`hcs::CancellationToken` in `StreamOptions`) and any error call `CancelIoEx`, drain the port until
  no read is pending, then join all threads. Nothing outlives the call; buffers are freed only after that.
- Raw chunks are verified in place (no extra copy); compressed chunks decode into a per-slot raw buffer.
- A read error, short read (truncated file) or removed device becomes `IoError`; corrupt data is `ModelError`.
- Plain files stream through the same machinery with synthesized Raw "chunks" and no hashes.

### HCSMODEL v1 layout (all little-endian)

Header, 288 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `HCSMODEL` |
| 8 | 4 | format_version (= 1) |
| 12 | 4 | header_size (= 288) |
| 16 | 4 | endian marker `0x01020304` (bytes 04 03 02 01) |
| 20 | 4 | header flags (must be 0) |
| 24 | 4 | nominal chunk size |
| 28 | 4 | reserved (0) |
| 32 | 8 | chunk table offset |
| 40 | 8 | chunk count (1 .. 2^20) |
| 48 | 8 | total raw size |
| 56 | 4 | table entry size (= 80) |
| 60 | 4 | reserved (0) |
| 64 | 64 | model id (zero padded text) |
| 128 | 32 | architecture |
| 160 | 32 | quantization |
| 192 | 32 | source manifest hash: SHA-256 of the complete logical stream |
| 224 | 32 | chunk table SHA-256 |
| 256 | 32 | header SHA-256 over bytes `[0, 256)` |

Chunk table entry, 80 bytes: `region_id u64`, `raw_offset u64`, `raw_size u64`, `file_offset u64`, `stored_size u64`,
`codec u32` (0 raw, 1 lz4, 2 zstd), `flags u32` (0), `sha256[32]` of the decoded chunk.

File: `[header][payload chunks...][chunk table]`. The decoded stream is the concatenation of all chunks and equals the
source file byte for byte.

Parser rules (every one has a test): validate magic, version, sizes and endian marker; reject unknown codec ids and
flags; chunk count / raw size / stored size caps; `offset + size` with checked arithmetic against the real file size;
no overlap between chunks, header and table; contiguous logical offsets that sum to the declared total; table and
header checksums; chunk hash on decode. A table that claims more entries than the file can hold is rejected before
any table-sized allocation.

Random access: `HcsModelReader::read_chunk(i)` / `read_range(off, n)` read and decode only the overlapping chunks
(proved in `RandomAccess.OneChunkDecodesWithoutTouchingOthers` by corrupting other chunks on disk).

Writer: `write_hcsmodel(src, dst, options)`: chunk size 64 KiB .. 256 MiB (default 16 MiB), codec + level,
raw fallback when savings are below `min_savings` (default 5 %), compression on up to 4 threads, SHA-256 per chunk and
for the manifest. Destination must not exist.

## CLI / benchmark

The benchmark executable is `hcs_storage_bench` (source `tools/benchmark-runner/storage_bench.cpp`, built by the
storage module; the shared `hcsinfer benchmark --suite storage` entry is left to the CLI owner and can call the same
`hcs::storage` API):

```powershell
build\<preset>\bin\hcs_storage_bench.exe --suite storage `
  --source E:\path\model.gguf --scratch <REPO>\build\storage-scratch `
  --out docs\test-evidence\M6-storage\storage-bench.json
```

`--scratch` must already exist; HCSMODEL files are created there as new files and deleted afterwards (`--keep` to
retain). The source is only read. Output follows the AGENTS.md 21.1 benchmark schema (version 1); fields that do
not apply to a storage benchmark (TTFT, tokens/s, GPU) are `null` / `n/a`, never invented.

## Measured results

Raw JSON: `docs/test-evidence/M6-storage/storage-bench-qwen3-0.6b-q4_k_m.json` (Release build, MSVC 19.44).
Machine: Ryzen 7 7735HS, 19.8 GB RAM, Windows 11 10.0.26200. Model: `qwen3-0.6b-q4_k_m.gguf` (Q4_K_M, 484,219,712 bytes,
sha256 `4ea79412...37ae7c`) on E:. Containers were written to `<REPO>\build\storage-scratch` (deleted afterwards).
**Both E: and D: are USB-attached disks (IB-1232CL-U3, MediaType "Unspecified"); whether they are HDD or SSD is unknown.**
Chunk size 16 MiB, 4 pipeline buffers, 3 workers, 1 warm-up + 3 measured runs, other engineers' builds were running.

| Measurement | Value | Notes |
|---|---|---|
| Source device speed, cache bypassed (`NO_BUFFERING`, E:) | **65.4 MiB/s** | cold-equivalent ceiling; the real limit for a first load |
| Scratch device speed, cache bypassed (D:) | 58 - 85 MiB/s | varies per container run |
| Source through the pipeline, file cache warm | ~3,850 MiB/s (0.12 s) | memory-speed; first pass in this process was already cached (1,482 MiB/s incl. SHA-256) |
| SHA-256, RAM resident | 1,556 MiB/s (SHA-NI) vs 312 MiB/s portable | 5x |

| Codec | Container size | vs raw | chunks stored compressed | Write time | Pipeline load (warm cache, RAM sink) | Decode from RAM, 1 thread (codec only / +SHA-256) | Bit-exact vs source |
|---|---|---|---|---|---|---|---|
| raw | 484,222,320 B | +0.0005 % | 0 / 29 | 5.8 s | 0.221 s (2,091 MiB/s) | 15,192 / 1,437 MiB/s | yes |
| lz4 (fast) | 480,011,454 B | **-0.87 %** | 2 / 29 | 7.1 s | 0.223 s (2,075 MiB/s) | 14,261 / 1,416 MiB/s | yes |
| zstd -3 | 477,525,557 B | **-1.38 %** | 3 / 29 | 8.7 s | 0.240 s (1,921 MiB/s) | 8,766 / 1,355 MiB/s | yes |

**Does compression help? Not for this Q4_K_M model.** Quantized weights are close to incompressible: 26-27 of 29
chunks failed the 5 % savings threshold and were stored Raw, and the whole file shrinks by 0.9 % (LZ4) to 1.4 % (Zstd).
With the measured 65 MiB/s device that is a *calculated* (not measured) saving of ~0.1 s on a ~7.1 s cold load,
well inside noise; on a warm cache the container is marginally slower (decode + hash costs 0.00 - 0.02 s).
Decode itself is never the bottleneck (>= 8.7 GiB/s codec-only per thread, ~1.4 GiB/s with SHA-256), so the
pipeline is device bound when the cache is cold. A real **cold end-to-end load was not measured** (the file cache cannot
be flushed without elevated tools); the warm numbers say nothing about the drive. Peak process RAM during the
benchmark was ~0.99 GB (includes the benchmark's own 0.46 GB destination buffer); pipeline buffers were 64 MiB (raw)
and 128 MiB (compressed) for 4 slots of 16 MiB chunks. The format therefore stays experimental and off by default; it
may pay off for less-quantized (F16/BF16) models, which were not measured here (NOT RUN).

## Measured results: upstream load modes (AGENTS.md 11.2 Stage A)

Before building an HCS-owned loading pipeline, the modes the pinned llama.cpp already offers were measured on the real
model from the USB drive (`HCSInfer benchmark --suite quick --load-mode <m>`, raw JSONs and the full caveat list in
`docs/test-evidence/M5-loadmodes/`):

| mode | cold load ms | warm load ms | warm decode t/s | peak working set MB | peak commit MB |
|---|---|---|---|---|---|
| `auto` (= mmap here) | 630 | 664 | 60.8 | 1106 | 1096 |
| `mmap` | 674 | 651 | 60.2 | 1106 | 1096 |
| `none` (read into RAM) | 768 | 655 | 59.7 | 984 | 1347 |
| `mlock` | 759 | 836 | 59.8 | 984 | 1347 |
| `mmap+mlock` | 900 | 871 | 59.3 | 1170 | 1097 |
| `direct_io` | **9538** | **9092** | 58.8 | 985 | 1348 |

Conclusions that the data supports: all six modes work; `direct_io` really bypasses the OS file cache and therefore
costs ~14x the load time on this drive (9.5 s vs 0.63 s, consistent with the 65 MiB/s raw device speed);
mmap-backed weights commit ~250 MB less than read-into-RAM modes; while the 0.48 GB model fits in RAM there is no
throughput difference to be found. The file cache was not controlled, so the load-time column is not a cold-cache
comparison except for `direct_io`. `--load-mode` is exposed by `chat`, `image`, `vision` and `benchmark`.

## Vault in the load path: `serve --vault-prefetch` (M5 Stage B, opt-in, default OFF)

The pinned llama.cpp and stable-diffusion.cpp loaders take a **path** (there is no caller-supplied reader callback), so
the vault cannot hand them decoded bytes. What it does instead, between the budget check and the engine call:

1. reads the model file (and the sibling mmproj, if one is loaded with it) once, front to back, through the bounded
   asynchronous reader (`storage::prefetch_and_verify`: overlapped I/O, `--vault-buffers` slots of `--vault-chunk-mib`,
   back-pressure, cancellable);
2. computes the SHA-256 on the way and compares it with the registry digest when one exists (`HCSInfer models hash <id>`
   records it; a mismatch aborts the load with a model error before the engine sees the file; an unhashed model is
   hashed but reported as `verified_against_manifest: false`);
3. hands the same path to the engine, which maps/reads it as before. Nothing is written anywhere: the bytes are hashed and
   dropped (no decompressed copy, no staging file). Pipeline RAM is fixed up front (4 x 16 MiB x 1 buffer set = 64 MiB
   by default).

The report is in `GET /hcs/v1/runtime` (`vault_prefetch`, `vault`). Unit/contract tests: `tests/unit/storage/test_vault_prefetch.cpp`,
`tests/contract/api/test_vault_load.cpp` (order, rollback, real digest, mismatch refusal; fake engine = plumbing only).

### Measured: real engine, real USB drive, cold and warm (N=3 per cell)

`tools/benchmark-runner/vault_load_bench.py`: a real `HCSInfer serve` process per run, LFM3-Omni-2B Q8_0 (1.85 GB) + its
mmproj (0.51 GB) from `D:\HCSModels` (D: is the external USB drive, "IB-1232CL-U3" enclosure), CPU backend, 3 repetitions
per cell interleaved. Cold = the files were evicted from the Windows file cache right before the load without
administrator rights (`tools/benchmark-runner/cache_evict.py`, unbuffered open; verified: warm read 2261 MiB/s, read after
eviction 45.7 MiB/s). Raw data: `docs/test-evidence/wave-b/e1/vault-load-usb-d.json`.

| mode | cache | load wall median (min..max) s | vault stage median s | peak working set | bytes written by the process |
|---|---|---|---|---|---|
| engine default (mmap) | cold | **56.2** (47.2..56.8) | - | 709 MiB | ~158 KiB (logs) |
| `--vault-prefetch` | cold | **33.6** (31.3..34.9) | 25.3 (93 MiB/s sequential) | 709 MiB | ~91 KiB |
| engine default (mmap) | warm | **1.49** (1.04..1.63) | - | 709 MiB | ~5 KiB |
| `--vault-prefetch` | warm | **3.27** (2.85..3.96) | 1.36 | 709 MiB | ~10 KiB |

What this supports, and what it does not:

- On this drive, a cold first load is ~1.7x faster with the vault stage: the engine's page-fault driven mmap reads the
  file at ~45 MiB/s, the sequential bounded reader at ~93 MiB/s, and the engine then maps warm pages. The ranges do not
  overlap (3 runs per cell is a small sample).
- A warm load is ~2x **slower** (the extra full read + SHA-256 pass, 1.4 s here). The vault cannot know whether the file
  is cached, so it stays **off by default**; turn it on for first loads from a slow external drive.
- The process wrote ~0.1-0.2 MiB in total in every mode (`GetProcessIoCounters`), i.e. no HCS-owned staging or decompressed
  copy exists. Peak working set did not change (the 64 MiB pipeline buffers are transient).
- Not measured: the second USB drive (E:), the Vulkan backend, a model larger than RAM (it would not change the
  mechanism: the engine still maps the file itself), the HCSMODEL container in this path (NOT RUN: the engines only open
  GGUF paths). Streaming straight into engine buffers needs an upstream loader hook and stays future work.
- Start-up and scan (same binary, this machine): process start to "listening" ~20-130 ms (`startup_s` in the JSON);
  `models scan` of the whole `D:\HCSModels` tree (37k files incl. a Python venv) fell from 3.6 s to 0.43 s warm after
  non-model files stopped being canonicalised (first-ever scan with a cold directory cache was 18.9 s before the change;
  not re-measurable cold afterwards). Scan never hashes. Evidence: `docs/test-evidence/wave-b/e1/README.md`.

## Fuzzing

- Always on: `tests/parser-fuzz/storage/test_mutation_fuzz.cpp` (deterministic mutation smoke test, label `parser`).
- Coverage-guided: `tests/parser-fuzz/storage/hcsmodel_fuzz.cpp` with libFuzzer
  (`-DHCS_BUILD_FUZZERS=ON` with a Clang toolchain that ships the fuzzer runtime). MSVC `cl` has no libFuzzer.
  Status of the local attempt is recorded in `docs/test-evidence/M6-storage/`.

## Known limitations

- Engines that memory-map the original GGUF (llama.cpp default) do not read through `stream_chunks`; the opt-in vault
  stage only pre-reads and verifies the file, a reader inside the engine's loader needs an upstream hook.
- Hash verification of Raw chunks costs one SHA-256 pass; disabling it is only allowed for plain files.
- The USB-unplug path is implemented as an `IoError` but a physical unplug test has not been run.
- Cold-cache measurements use `FILE_FLAG_NO_BUFFERING` probes for the device speed; end-to-end runs report the
  cache state as "uncontrolled" because the cache cannot be flushed without elevated tools.
