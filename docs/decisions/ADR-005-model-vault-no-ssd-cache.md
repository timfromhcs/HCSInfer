# ADR-005: Model Vault and the no-SSD-app-cache policy

Status: Accepted for V1 scope (2026-11-17). Implemented in `src/storage` (`hcs_storage`).

## Context
Models live on an external USB drive (cold archive). The product principle (AGENTS.md 11) is: bounded asynchronous
sequential reads into RAM, no HCS-owned decompressed model cache or staging file on the SSD by default, and honest
claims (an HDD is not RAM; Windows may still page memory to its page file).

## Decision
1. **Stage A (upstream first).** Original GGUF/safetensors stay the canonical formats. The vault validates them
   read-only (`PlainFileReader`, safe GGUF header parser) and never copies or rewrites them. Load modes of
   llama.cpp (`mmap`, `mlock`, direct I/O) and sd.cpp parameter placement are benchmarked separately by their owners.
2. **Stage B (bounded pipeline).** `stream_chunks()` reads the source with Win32 overlapped I/O
   (`CreateFileW(FILE_FLAG_OVERLAPPED)` + I/O completion port), into N fixed slot buffers, decodes and verifies on
   worker threads and hands chunks to a sink in order. Memory is `N * (max stored chunk + max raw chunk)` and is
   allocated once before the first read; an explicit `max_buffer_bytes` budget fails with `OutOfMemory` before
   allocation. A slow sink back-pressures reads (a slot is reused only after the sink returned).
3. **No decompressed cache on disk.** Decoded bytes exist only in the slot buffers and in whatever RAM destination
   the sink owns. HCS code never writes decoded model data to any file. The only writer is the opt-in
   `write_hcsmodel`, which creates a NEW container file (refuses to overwrite, removes only its own partial file on
   failure) in a directory the caller names; the benchmark requires an explicit `--scratch` directory and deletes only
   the files it created. Nothing defaults to the SSD or to a profile directory.
4. **Page file.** HCSInfer does not disable, resize or bypass the Windows page file. Documentation states that
   resident model memory may be paged out by the OS.
5. **Cancellation and device loss.** Cancellation cancels outstanding overlapped reads (`CancelIoEx`) and drains the
   port before buffers are freed. A read error (device removed, short read, truncated file) fails the stream with
   `IoError`; no thread or handle outlives the call.

## Rejected
- A hidden decompress-to-SSD staging cache (violates the policy and wears the SSD).
- Disabling or shrinking the page file to make the "never pages" claim true.
- `FILE_FLAG_NO_BUFFERING` as the streaming default: it needs sector-aligned offsets/sizes that the chunk table does
  not guarantee; it is used only for device-speed probes in the benchmark.
- Mandatory `FILE_FLAG_SEQUENTIAL_SCAN`: left as an option (`StreamOptions::sequential_hint`) until measured.

## Risks
Whether bounded streaming helps real model load time depends on the drive; on a fast device the pipeline is
decode/hash bound (see `docs/test-evidence/M6-storage/`). Engines that memory-map the original GGUF bypass this
pipeline entirely; integrating the reader with an engine's loader is a separate adapter task.

## Evidence
`tests/unit/storage/test_stream.cpp` (bounds, back-pressure, cancellation, error propagation),
`tests/unit/storage/test_roundtrip.cpp`, benchmark JSON under `docs/test-evidence/M6-storage/`.
USB-unplug during load: error path implemented and unit-tested only for corrupt/short data; a physical unplug test is
NOT RUN.
