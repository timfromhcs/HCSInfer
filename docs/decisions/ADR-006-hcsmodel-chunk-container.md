# ADR-006: HCSMODEL chunk container, versioning and checksums

Status: Accepted as EXPERIMENTAL (2026-11-17). Implemented in `src/storage/hcsmodel.cpp`, `writer.cpp`.

## Context
AGENTS.md 11.4 asks for an optional wrapper (not a replacement for GGUF/safetensors) with chunked, independently
decodable, integrity-checked storage, Raw/LZ4/Zstd codecs and a parser that treats the file as hostile.

## Decision
Layout (little-endian, `docs/model-vault.md` has the byte table): fixed 288-byte header, payload chunks, chunk table
last (so the writer can stream; the header points at the table).

- **Versioning:** `format_version = 1`; any other value is `Unsupported`. `header_size` is checked (a future
  version may grow it). Endianness marker `0x01020304` must read back identically; header flags and per-chunk flags
  must be 0 (unknown => `Unsupported`); reserved fields must be 0.
- **Integrity:** SHA-256 is used everywhere: per chunk over the DECODED bytes, over the whole chunk table, over the
  header (first 256 bytes) and over the complete source stream (`source_manifest_hash`).
  Why SHA-256 and not xxHash: tamper evidence, not just accidental-corruption detection, with no extra
  dependency; implemented in-tree and verified against NIST vectors, with an SHA-NI path (runtime-detected, tested
  equal to the portable path). Measured cost on the target CPU is in the benchmark evidence.
- **Chunks:** 80-byte entries: region id, logical offset, raw size, file offset, stored size, codec, flags, hash.
  Chunks are independently decodable, so one chunk can be read and decoded without touching any other
  (`HcsModelReader::read_chunk`, `read_range`). Logical offsets must be contiguous and sum to `total_raw_size`.
- **Caps (validated before any allocation):** chunk count <= 2^20, raw chunk size <= 256 MiB, nominal chunk size
  <= 256 MiB, stored size <= raw + raw/255 + 1024, table size = count*80 must lie inside the actual file size.
  Offset+size math is checked (`checked_add/checked_mul/range_within`). Chunk payloads must not overlap each other,
  the header or the table (sorted sweep, O(n log n)).
- **Codecs:** Raw, LZ4 (v1.10.0), Zstandard (v1.5.7), statically linked, decoders bounded by the declared raw size.
  The writer stores a chunk compressed only if `stored <= raw * (1 - min_savings)` (default 5 %), else Raw.
  Lossless only; quantization (model representation) is separate from storage compression.
- **Chunk size:** default 16 MiB; the benchmark takes `--chunk-mib` (candidates 4..64 MiB are an experiment, not
  an optimum).
- **Writer safety:** creates a new file with `wbx` semantics, refuses to overwrite, deletes only its own partial file.

## Rejected
Header-embedded variable-length strings (fixed fields keep the parser trivial); table-first layout (needs the
chunk sizes before writing the payload); a per-chunk CRC32 (no tamper evidence); lossy compression of any kind.

## Risks
Chunk hash covers decoded data, so Raw chunks are hashed at ~GB/s and compressed chunks pay decode + hash.
The container only pays off if the source is I/O bound AND the data compresses. For Q4_K_M GGUF it does not
(measured, see evidence); the format stays experimental and is not enabled by default anywhere.
Migration: a v2 reader must keep reading v1; readers reject unknown versions rather than guessing.

## Evidence
Parser: `tests/unit/storage/test_hcsmodel_parser.cpp` (truncation, overflow, overlap, bad codec/flags/hash, huge
declared sizes, corrupt blocks), mutation fuzz `tests/parser-fuzz/storage/test_mutation_fuzz.cpp`, libFuzzer
harness `hcsmodel_fuzz.cpp` (status in `docs/test-evidence/M6-storage/`).
