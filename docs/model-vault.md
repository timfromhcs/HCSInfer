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
  --source E:\path\model.gguf --scratch D:\HCSInfer\build\storage-scratch `
  --out docs\test-evidence\M6-storage\storage-bench.json
```

`--scratch` must already exist; HCSMODEL files are created there as new files and deleted afterwards (`--keep` to
retain). The source is only read. Output follows the AGENTS.md 21.1 benchmark schema (version 1); fields that do
not apply to a storage benchmark (TTFT, tokens/s, GPU) are `null` / `n/a`, never invented.

## Measured results

Raw JSON: `docs/test-evidence/M6-storage/storage-bench-qwen3-0.6b-q4_k_m.json` (Release build, MSVC 19.44).
Machine: Ryzen 7 7735HS, 19.8 GB RAM, Windows 11 10.0.26200. Model: `qwen3-0.6b-q4_k_m.gguf` (Q4_K_M, 484,219,712 bytes,
sha256 `4ea79412...37ae7c`) on E:. Containers were written to `D:\HCSInfer\build\storage-scratch` (deleted afterwards).
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
comparison except for `direct_io`. Stage B of the vault - a bounded asynchronous pipeline feeding the engine - is
**BLOCKED** on this machine: it needs a model that does not fit in RAM and/or an administrator-assisted cache flush to
produce a meaningful measurement (see the evidence README). `--load-mode` is exposed by `chat`, `image`, `vision` and
`benchmark`.

## Fuzzing

- Always on: `tests/parser-fuzz/storage/test_mutation_fuzz.cpp` (deterministic mutation smoke test, label `parser`).
- Coverage-guided: `tests/parser-fuzz/storage/hcsmodel_fuzz.cpp` with libFuzzer
  (`-DHCS_BUILD_FUZZERS=ON` with a Clang toolchain that ships the fuzzer runtime). MSVC `cl` has no libFuzzer.
  Status of the local attempt is recorded in `docs/test-evidence/M6-storage/`.

## Known limitations

- Engines that memory-map the original GGUF (llama.cpp default) do not go through `stream_chunks`; wiring the
  reader into an engine's loader is future adapter work.
- Hash verification of Raw chunks costs one SHA-256 pass; disabling it is only allowed for plain files.
- The USB-unplug path is implemented as an `IoError` but a physical unplug test has not been run.
- Cold-cache measurements use `FILE_FLAG_NO_BUFFERING` probes for the device speed; end-to-end runs report the
  cache state as "uncontrolled" because the cache cannot be flushed without elevated tools.
