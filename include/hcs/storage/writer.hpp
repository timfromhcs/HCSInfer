#pragma once
// HCSMODEL writer. Creates a NEW file only (never overwrites); on failure the partial file it
// created is removed. Callers choose the destination: the Model Vault never defaults to the SSD.
#include <cstdint>
#include <filesystem>
#include <string>

#include "hcs/job_queue.hpp"
#include "hcs/result.hpp"
#include "hcs/storage/codec.hpp"
#include "hcs/storage/hcsmodel.hpp"

namespace hcs::storage {

struct WriteOptions {
    Codec codec = Codec::Raw;               // preferred codec
    int level = 0;                          // 0 = codec default (Zstd 3, LZ4 fast)
    std::uint32_t chunk_size = 16u << 20;   // raw bytes per chunk, [kMinWriterChunkSize, kMaxChunkRawSize]
    // A chunk is stored compressed only if comp_size <= raw_size * (1 - min_savings), else as Raw.
    double min_savings = 0.05;
    std::size_t threads = 0;                // compression threads; 0 = min(hardware, 4)
    std::string model_id, architecture, quantization;  // empty => derived / "unknown"
    hcs::CancellationToken cancel;
};

struct WriteStats {
    std::uint64_t raw_bytes = 0;
    std::uint64_t container_bytes = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t chunks = 0, chunks_raw = 0, chunks_lz4 = 0, chunks_zstd = 0;
    double seconds = 0;
    double compress_cpu_seconds = 0;
    double source_read_seconds = 0;
    double ratio() const { return raw_bytes ? static_cast<double>(container_bytes) / static_cast<double>(raw_bytes) : 1.0; }
    std::string source_sha256_hex;
};

// `src` is read sequentially (read-only). `dst` must not exist.
Result<WriteStats> write_hcsmodel(const std::filesystem::path& src, const std::filesystem::path& dst,
                                  const WriteOptions& options);

}  // namespace hcs::storage
