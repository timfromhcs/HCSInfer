#pragma once
// Model Vault load-path stage (M5 Stage B, opt-in): reads a model file ONCE, front to back, through the
// bounded asynchronous reader (overlapped I/O, `buffers` slots of `chunk_bytes`, back-pressure) and
// computes its SHA-256 on the way. Nothing is written anywhere: the bytes are hashed and dropped, so there is
// no HCS-owned staging file and peak buffer RAM is fixed up front (buffers * chunk_bytes).
//
// What it does for the engine: the sequential pass leaves the file in the OS page cache and proves the
// bytes match the expected digest BEFORE the engine maps/reads them. The pinned llama.cpp /
// stable-diffusion.cpp loaders take a path (no caller-supplied reader callback), so the engine itself still
// opens the file; whether the warm-up shortens end-to-end load is a measurement (docs/model-vault.md), not an
// assumption.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "hcs/job_queue.hpp"
#include "hcs/result.hpp"

namespace hcs::storage {

struct PrefetchOptions {
    std::size_t buffers = 4;                // slots in flight (1..64); also the read-ahead depth
    std::uint32_t chunk_bytes = 16u << 20;  // read granularity (1 MiB..256 MiB)
    std::string expected_sha256;            // lower-case hex; empty => the digest is computed, not compared
    bool sequential_hint = true;            // FILE_FLAG_SEQUENTIAL_SCAN on the streaming handle
    hcs::CancellationToken cancel;
};

struct PrefetchReport {
    std::uint64_t bytes = 0;
    std::string sha256;     // hex of everything read
    bool verified = false;  // expected_sha256 was given and matched
    double wall_seconds = 0;
    double mib_per_second = 0;
    std::uint64_t buffer_bytes = 0;  // upper bound of the pipeline's RAM (does not grow with the file size)
};

// Errors: InvalidArgument (options), IoError/NotFound (open/read), Cancelled, ModelError (digest mismatch;
// the message names expected and actual digest, never any file content).
hcs::Result<PrefetchReport> prefetch_and_verify(const std::filesystem::path& file,
                                                const PrefetchOptions& options);

}  // namespace hcs::storage
