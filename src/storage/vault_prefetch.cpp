#include "hcs/storage/vault_prefetch.hpp"

#include <algorithm>
#include <vector>

#include "hcs/storage/file_source.hpp"
#include "hcs/storage/hcsmodel.hpp"
#include "hcs/storage/sha256.hpp"
#include "hcs/storage/stream.hpp"

namespace hcs::storage {

hcs::Result<PrefetchReport> prefetch_and_verify(const std::filesystem::path& file,
                                                const PrefetchOptions& options) {
    if (options.buffers == 0 || options.buffers > 64)
        return Error(ErrorCode::InvalidArgument, "vault prefetch: buffers must be 1..64");
    if (options.chunk_bytes < (1u << 20) || options.chunk_bytes > (256u << 20))
        return Error(ErrorCode::InvalidArgument, "vault prefetch: chunk size must be 1..256 MiB");
    Digest expected{};
    const bool compare = !options.expected_sha256.empty();
    if (compare && !digest_from_hex(options.expected_sha256, expected))
        return Error(ErrorCode::InvalidArgument, "vault prefetch: expected digest is not 64 hex characters");

    auto src = FileSource::open(file, options.sequential_hint);
    if (!src)
        return src.error();
    const FileSource& fs = *src.value();
    const std::uint64_t size = fs.size();
    if (size == 0)
        return Error(ErrorCode::ModelError, "model file is empty");
    const std::uint64_t count = (size + options.chunk_bytes - 1) / options.chunk_bytes;
    if (count > (1ull << 20))
        return Error(ErrorCode::InvalidArgument, "vault prefetch: file too large for the chunk size");

    std::vector<ChunkEntry> entries(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        ChunkEntry& e = entries[static_cast<std::size_t>(i)];
        e.region_id = i;
        e.raw_offset = i * options.chunk_bytes;
        e.offset = e.raw_offset;
        e.raw_size = e.comp_size = std::min<std::uint64_t>(options.chunk_bytes, size - e.raw_offset);
        e.codec = Codec::Raw;
    }

    StreamOptions so;
    so.buffers = options.buffers;
    so.workers = 1;
    so.verify_hash = false;  // plain file: no per-chunk hashes; the whole-file digest is computed below
    so.cancel = options.cancel;
    so.sequential_hint = options.sequential_hint;

    Sha256 h;
    auto st =
        stream_chunks(fs, entries, so, [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t> d) {
            h.update(d);
            return hcs::Result<void>{};
        });
    if (!st)
        return st.error();
    PrefetchReport rep;
    rep.bytes = st.value().raw_bytes;
    rep.sha256 = to_hex(h.finalize());
    rep.wall_seconds = st.value().wall_seconds;
    rep.mib_per_second =
        rep.wall_seconds > 0 ? static_cast<double>(rep.bytes) / (1024.0 * 1024.0) / rep.wall_seconds : 0;
    rep.buffer_bytes = st.value().buffer_bytes_allocated;
    if (rep.bytes != size)
        return Error(ErrorCode::IoError, "vault prefetch: read " + std::to_string(rep.bytes) + " of " +
                                             std::to_string(size) + " bytes");
    if (compare) {
        if (rep.sha256 != to_hex(expected))
            return Error(ErrorCode::ModelError, "model file digest mismatch: expected " +
                                                    options.expected_sha256 + ", got " + rep.sha256);
        rep.verified = true;
    }
    return rep;
}

}  // namespace hcs::storage
