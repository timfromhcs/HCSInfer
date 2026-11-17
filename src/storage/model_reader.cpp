#include "hcs/storage/model_reader.hpp"

#include <algorithm>
#include <cstring>

#include "hcs/checked_math.hpp"

namespace hcs::storage {

// ---------------------------------------------------------------- PlainFileReader

Result<std::unique_ptr<PlainFileReader>> PlainFileReader::open(const std::filesystem::path& path, std::uint32_t chunk_size) {
    if (chunk_size < kMinWriterChunkSize || chunk_size > kMaxChunkRawSize)
        return Error(ErrorCode::InvalidArgument, "chunk size out of range");
    auto f = FileSource::open(path);
    if (!f) return f.error();
    std::unique_ptr<PlainFileReader> r(new PlainFileReader());
    r->file_ = std::move(f).value();
    r->chunk_size_ = chunk_size;
    if (r->file_->size() == 0) return Error(ErrorCode::ModelError, "model file is empty");
    char magic[4] = {};
    if (r->file_->size() >= 4) {
        if (auto m = r->file_->read_at(0, magic, 4); !m) return m.error();
    }
    if (std::memcmp(magic, "GGUF", 4) == 0) {
        auto g = parse_gguf(*r->file_);
        if (!g) return g.error();
        r->gguf_ = std::move(g).value();
    }
    return r;
}

Result<void> PlainFileReader::read_range(std::uint64_t offset, std::size_t n, void* dst) const {
    return file_->read_at(offset, dst, n);
}

Result<StreamStats> PlainFileReader::stream(const ChunkSink& sink, const StreamOptions& options) const {
    const std::uint64_t size = file_->size();
    const std::uint64_t count = (size + chunk_size_ - 1) / chunk_size_;
    if (count > kMaxChunkCount) return Error(ErrorCode::InvalidArgument, "file too large for the chunk size; use a bigger chunk size");
    std::vector<ChunkEntry> entries(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        ChunkEntry& e = entries[static_cast<std::size_t>(i)];
        e.region_id = i;
        e.raw_offset = i * chunk_size_;
        e.offset = e.raw_offset;
        e.raw_size = e.comp_size = std::min<std::uint64_t>(chunk_size_, size - e.raw_offset);
        e.codec = Codec::Raw;
    }
    StreamOptions o = options;
    o.verify_hash = false;  // a plain file carries no per-chunk hashes
    return stream_chunks(*file_, entries, o, sink);
}

// ---------------------------------------------------------------- HcsModelReader

Result<std::unique_ptr<HcsModelReader>> HcsModelReader::open(const std::filesystem::path& path, const ParseOptions& opt) {
    auto f = FileSource::open(path);
    if (!f) return f.error();
    std::unique_ptr<HcsModelReader> r(new HcsModelReader());
    r->file_ = std::move(f).value();
    const std::uint64_t fsize = r->file_->size();
    if (fsize < kHeaderSize) return Error(ErrorCode::ModelError, "HCSMODEL: file shorter than the fixed header (truncated)");
    std::vector<std::uint8_t> hb(kHeaderSize);
    if (auto rd = r->file_->read_at(0, hb.data(), hb.size()); !rd) return rd.error();
    auto h = parse_header(hb, fsize, opt);
    if (!h) return h.error();
    r->header_ = std::move(h).value();
    // parse_header proved table_size == count * 80 lies inside the file, so this allocation is bounded by the file size.
    const std::uint64_t table_size = r->header_.chunk_count * kChunkEntrySize;
    std::vector<std::uint8_t> tb(static_cast<std::size_t>(table_size));
    if (auto rd = r->file_->read_at(r->header_.chunk_table_offset, tb.data(), tb.size()); !rd) return rd.error();
    auto t = parse_chunk_table(tb, r->header_, fsize, opt);
    if (!t) return t.error();
    r->chunks_ = std::move(t).value();
    return r;
}

Result<void> HcsModelReader::read_chunk(std::size_t index, std::vector<std::uint8_t>& out) const {
    if (index >= chunks_.size()) return Error(ErrorCode::InvalidArgument, "chunk index out of range");
    const ChunkEntry& e = chunks_[index];
    std::vector<std::uint8_t> stored(static_cast<std::size_t>(e.comp_size));
    if (auto r = file_->read_at(e.offset, stored.data(), stored.size()); !r) return r;
    out.resize(static_cast<std::size_t>(e.raw_size));
    auto r = decode_chunk(e, stored, out, true);
    if (!r) return Error(r.error().code, r.error().message + " (chunk " + std::to_string(index) + ")");
    return {};
}

Result<void> HcsModelReader::read_range(std::uint64_t offset, std::size_t n, void* dst) const {
    if (!range_within(offset, n, header_.total_raw_size)) return Error(ErrorCode::InvalidArgument, "range outside the logical stream");
    auto* out = static_cast<std::uint8_t*>(dst);
    // Binary search the first chunk containing `offset` (raw offsets are contiguous and sorted).
    auto it = std::upper_bound(chunks_.begin(), chunks_.end(), offset,
                               [](std::uint64_t off, const ChunkEntry& c) { return off < c.raw_offset; });
    std::size_t idx = static_cast<std::size_t>(it - chunks_.begin()) - 1;
    std::vector<std::uint8_t> buf;
    while (n) {
        if (auto r = read_chunk(idx, buf); !r) return r;
        const ChunkEntry& e = chunks_[idx];
        const std::size_t inner = static_cast<std::size_t>(offset - e.raw_offset);
        const std::size_t take = std::min<std::size_t>(n, buf.size() - inner);
        std::memcpy(out, buf.data() + inner, take);
        out += take;
        offset += take;
        n -= take;
        ++idx;
    }
    return {};
}

Result<StreamStats> HcsModelReader::stream(const ChunkSink& sink, const StreamOptions& options) const {
    return stream_chunks(*file_, chunks_, options, sink);
}

// ---------------------------------------------------------------- factory

Result<std::unique_ptr<IModelReader>> open_model_reader(const std::filesystem::path& path) {
    auto f = FileSource::open(path);
    if (!f) return f.error();
    char magic[8] = {};
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(8, f.value()->size()));
    if (n) {
        if (auto r = f.value()->read_at(0, magic, n); !r) return r.error();
    }
    f = Error(ErrorCode::Internal, "released");  // close the sniffing handle before re-opening
    if (std::memcmp(magic, kMagic, 8) == 0) {
        auto r = HcsModelReader::open(path);
        if (!r) return r.error();
        return std::unique_ptr<IModelReader>(std::move(r).value());
    }
    auto r = PlainFileReader::open(path);
    if (!r) return r.error();
    return std::unique_ptr<IModelReader>(std::move(r).value());
}

}  // namespace hcs::storage
