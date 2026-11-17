#pragma once
// Bounded-RAM streaming pipeline:
//   overlapped reads (IOCP) -> N slot buffers -> decode/verify workers -> in-order sink.
// Peak buffer memory is fixed up front: buffers * (max stored chunk + max raw chunk). A slow sink
// back-pressures the reader (a slot is re-used only after the sink returned for its chunk).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>

#include "hcs/job_queue.hpp"
#include "hcs/result.hpp"
#include "hcs/storage/file_source.hpp"
#include "hcs/storage/hcsmodel.hpp"

namespace hcs::storage {

struct StreamOptions {
    std::size_t buffers = 4;       // slots in flight (1..64)
    std::size_t workers = 0;       // decode threads; 0 = min(buffers, 3)
    bool verify_hash = true;       // false only for plain (non-HCSMODEL) files
    std::size_t begin_chunk = 0;
    std::size_t end_chunk = std::numeric_limits<std::size_t>::max();  // clamped to table size
    std::uint64_t max_buffer_bytes = 0;  // 0 = unlimited; otherwise OutOfMemory before allocating
    hcs::CancellationToken cancel;
    bool sequential_hint = false;  // FILE_FLAG_SEQUENTIAL_SCAN on the streaming handle
};

struct StreamStats {
    std::uint64_t chunks = 0;
    std::uint64_t raw_bytes = 0;
    std::uint64_t stored_bytes_read = 0;
    std::uint64_t buffer_bytes_allocated = 0;  // upper bound of pipeline buffer memory
    double wall_seconds = 0;
    double consumer_wait_seconds = 0;  // time the consumer waited for data (I/O+decode bound if large)
};

// Called on the calling thread, strictly in chunk order. `data` is valid only during the call.
using ChunkSink = std::function<hcs::Result<void>(std::size_t index, const ChunkEntry& entry,
                                                  std::span<const std::uint8_t> data)>;

hcs::Result<StreamStats> stream_chunks(const FileSource& file, std::span<const ChunkEntry> chunks,
                                       const StreamOptions& options, const ChunkSink& sink);

}  // namespace hcs::storage
