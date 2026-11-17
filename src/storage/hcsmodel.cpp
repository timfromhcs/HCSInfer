#include "hcs/storage/hcsmodel.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>

#include "hcs/checked_math.hpp"

namespace hcs::storage {
namespace {

// Field offsets of the 288-byte header.
constexpr std::size_t kOffMagic = 0, kOffVersion = 8, kOffHeaderSize = 12, kOffEndian = 16, kOffFlags = 20,
                      kOffChunkSize = 24, kOffReserved0 = 28, kOffTableOffset = 32, kOffChunkCount = 40,
                      kOffTotalRaw = 48, kOffEntrySize = 56, kOffReserved1 = 60, kOffModelId = 64,
                      kOffArch = 128, kOffQuant = 160, kOffManifest = 192, kOffTableHash = 224,
                      kOffHeaderHash = 256;
static_assert(kOffHeaderHash + 32 == kHeaderSize);
static_assert(kOffModelId + kIdFieldSize == kOffArch);
static_assert(kOffArch + kTextFieldSize == kOffQuant);

std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
std::uint64_t get64(const std::uint8_t* p) {
    return std::uint64_t(get32(p)) | (std::uint64_t(get32(p + 4)) << 32);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

Error bad(std::string m) {
    return Error(ErrorCode::ModelError, "HCSMODEL: " + std::move(m));
}
Error unsupported(std::string m) {
    return Error(ErrorCode::Unsupported, "HCSMODEL: " + std::move(m));
}

// Zero-padded text field: only printable-ish bytes before the first NUL, only NULs after it.
Result<std::string> read_text(const std::uint8_t* p, std::size_t cap, const char* what) {
    std::size_t len = 0;
    while (len < cap && p[len] != 0) ++len;
    for (std::size_t i = len; i < cap; ++i)
        if (p[i] != 0)
            return bad(std::string(what) + " field has data after terminator");
    for (std::size_t i = 0; i < len; ++i)
        if (p[i] < 0x20 || p[i] == 0x7F)
            return bad(std::string(what) + " field contains control characters");
    return std::string(reinterpret_cast<const char*>(p), len);
}

void write_text(std::uint8_t* p, std::size_t cap, const std::string& s) {
    std::memset(p, 0, cap);
    std::memcpy(p, s.data(), std::min(s.size(), cap - 1));
}

// Largest stored size any codec can legitimately produce for `raw` bytes (generous, codec-agnostic).
std::uint64_t max_stored_size(std::uint64_t raw) {
    return raw + raw / 255 + 1024;
}

}  // namespace

Result<HcsHeader> parse_header(std::span<const std::uint8_t> b, std::uint64_t file_size,
                               const ParseOptions& opt) {
    if (b.size() < kHeaderSize || file_size < kHeaderSize)
        return bad("file shorter than the fixed header (truncated)");
    const std::uint8_t* p = b.data();
    if (std::memcmp(p + kOffMagic, kMagic, 8) != 0)
        return bad("bad magic");
    const std::uint32_t version = get32(p + kOffVersion);
    if (version != kFormatVersion)
        return unsupported("unsupported format version " + std::to_string(version));
    if (get32(p + kOffHeaderSize) != kHeaderSize)
        return bad("unexpected header size");
    if (get32(p + kOffEndian) != kEndianMarker)
        return bad("endianness marker mismatch");
    if (get32(p + kOffFlags) != 0)
        return unsupported("unknown header flags");
    if (get32(p + kOffReserved0) != 0 || get32(p + kOffReserved1) != 0)
        return bad("reserved fields must be zero");
    if (get32(p + kOffEntrySize) != kChunkEntrySize)
        return bad("unexpected chunk table entry size");

    if (opt.verify_checksums) {
        const Digest h = sha256(std::span<const std::uint8_t>(p, kOffHeaderHash));
        if (std::memcmp(h.data(), p + kOffHeaderHash, 32) != 0)
            return bad("header checksum mismatch");
    }

    HcsHeader h;
    h.format_version = version;
    h.nominal_chunk_size = get32(p + kOffChunkSize);
    h.chunk_table_offset = get64(p + kOffTableOffset);
    h.chunk_count = get64(p + kOffChunkCount);
    h.total_raw_size = get64(p + kOffTotalRaw);

    if (h.chunk_count == 0 || h.chunk_count > kMaxChunkCount)
        return bad("chunk count out of range");
    if (h.nominal_chunk_size == 0 || h.nominal_chunk_size > kMaxChunkRawSize)
        return bad("nominal chunk size out of range");
    // chunk_count <= 2^20 and each chunk <= 2^28 bytes, so this product cannot overflow, but stay explicit.
    const auto max_total = checked_mul(h.chunk_count, kMaxChunkRawSize);
    if (!max_total || h.total_raw_size == 0 || h.total_raw_size > *max_total)
        return bad("total raw size out of range");

    const auto table_size = checked_mul(h.chunk_count, kChunkEntrySize);
    if (!table_size)
        return bad("chunk table size overflow");
    if (h.chunk_table_offset < kHeaderSize)
        return bad("chunk table overlaps the header");
    if (!range_within(h.chunk_table_offset, *table_size, file_size))
        return bad("chunk table outside the file (truncated)");

    auto id = read_text(p + kOffModelId, kIdFieldSize, "model_id");
    if (!id)
        return id.error();
    auto arch = read_text(p + kOffArch, kTextFieldSize, "architecture");
    if (!arch)
        return arch.error();
    auto quant = read_text(p + kOffQuant, kTextFieldSize, "quantization");
    if (!quant)
        return quant.error();
    h.model_id = std::move(id).value();
    h.architecture = std::move(arch).value();
    h.quantization = std::move(quant).value();
    std::memcpy(h.source_manifest_hash.data(), p + kOffManifest, 32);
    std::memcpy(h.chunk_table_hash.data(), p + kOffTableHash, 32);
    return h;
}

Result<std::vector<ChunkEntry>> parse_chunk_table(std::span<const std::uint8_t> table, const HcsHeader& h,
                                                  std::uint64_t file_size, const ParseOptions& opt) {
    if (h.chunk_count == 0 || h.chunk_count > kMaxChunkCount)
        return bad("chunk count out of range");
    const auto table_size = checked_mul(h.chunk_count, kChunkEntrySize);
    if (!table_size || table.size() != *table_size)
        return bad("chunk table size mismatch");
    if (h.chunk_table_offset < kHeaderSize || !range_within(h.chunk_table_offset, *table_size, file_size))
        return bad("chunk table outside the file (truncated)");
    if (opt.verify_checksums) {
        const Digest d = sha256(table);
        if (d != h.chunk_table_hash)
            return bad("chunk table checksum mismatch");
    }

    std::vector<ChunkEntry> out;
    out.reserve(static_cast<std::size_t>(h.chunk_count));  // safe: validated against file size above
    std::uint64_t running = 0;
    const std::uint64_t table_begin = h.chunk_table_offset, table_end = h.chunk_table_offset + *table_size;
    for (std::uint64_t i = 0; i < h.chunk_count; ++i) {
        const std::uint8_t* p = table.data() + i * kChunkEntrySize;
        ChunkEntry e;
        e.region_id = get64(p);
        e.raw_offset = get64(p + 8);
        e.raw_size = get64(p + 16);
        e.offset = get64(p + 24);
        e.comp_size = get64(p + 32);
        const std::uint32_t codec_id = get32(p + 40);
        e.flags = get32(p + 44);
        std::memcpy(e.hash.data(), p + 48, 32);

        const auto codec = codec_from_id(codec_id);
        if (!codec)
            return unsupported("unknown codec id " + std::to_string(codec_id) + " in chunk " +
                               std::to_string(i));
        e.codec = *codec;
        if (e.flags != 0)
            return unsupported("unknown chunk flags in chunk " + std::to_string(i));
        if (e.raw_size == 0 || e.raw_size > kMaxChunkRawSize)
            return bad("chunk " + std::to_string(i) + " raw size out of range");
        if (e.raw_size > h.nominal_chunk_size)
            return bad("chunk " + std::to_string(i) + " larger than the nominal chunk size");
        if (e.raw_offset != running)
            return bad("chunk " + std::to_string(i) + " is not contiguous in the logical stream");
        if (e.comp_size == 0 || e.comp_size > max_stored_size(e.raw_size))
            return bad("chunk " + std::to_string(i) + " stored size out of range");
        if (e.codec == Codec::Raw && e.comp_size != e.raw_size)
            return bad("raw chunk " + std::to_string(i) + " size mismatch");
        const auto end = checked_add(e.offset, e.comp_size);
        if (!end)
            return bad("chunk " + std::to_string(i) + " offset+size overflows");
        if (e.offset < kHeaderSize || *end > file_size)
            return bad("chunk " + std::to_string(i) + " outside the file (truncated)");
        if (e.offset < table_end && *end > table_begin)
            return bad("chunk " + std::to_string(i) + " overlaps the chunk table");
        const auto next = checked_add(running, e.raw_size);
        if (!next)
            return bad("logical size overflow");
        running = *next;
        out.push_back(e);
    }
    if (running != h.total_raw_size)
        return bad("chunk sizes do not add up to the declared total size");

    // Overlap rejection among payloads (O(n log n)).
    std::vector<std::uint32_t> order(out.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(),
              [&](std::uint32_t a, std::uint32_t b) { return out[a].offset < out[b].offset; });
    for (std::size_t k = 1; k < order.size(); ++k) {
        const auto& prev = out[order[k - 1]];
        const auto& cur = out[order[k]];
        if (prev.offset + prev.comp_size > cur.offset)  // both sums were range-checked above
            return bad("chunk payloads overlap (chunks " + std::to_string(order[k - 1]) + " and " +
                       std::to_string(order[k]) + ")");
    }
    return out;
}

Result<ParsedContainer> parse_container_image(std::span<const std::uint8_t> image, const ParseOptions& opt) {
    auto h = parse_header(image, image.size(), opt);
    if (!h)
        return h.error();
    const std::uint64_t table_size = h.value().chunk_count * kChunkEntrySize;  // validated by parse_header
    auto entries = parse_chunk_table(image.subspan(static_cast<std::size_t>(h.value().chunk_table_offset),
                                                   static_cast<std::size_t>(table_size)),
                                     h.value(), image.size(), opt);
    if (!entries)
        return entries.error();
    return ParsedContainer{std::move(h).value(), std::move(entries).value()};
}

std::vector<std::uint8_t> serialize_header(const HcsHeader& h) {
    std::vector<std::uint8_t> b(kHeaderSize, 0);
    std::uint8_t* p = b.data();
    std::memcpy(p + kOffMagic, kMagic, 8);
    put32(p + kOffVersion, h.format_version);
    put32(p + kOffHeaderSize, kHeaderSize);
    put32(p + kOffEndian, kEndianMarker);
    put32(p + kOffFlags, 0);
    put32(p + kOffChunkSize, h.nominal_chunk_size);
    put64(p + kOffTableOffset, h.chunk_table_offset);
    put64(p + kOffChunkCount, h.chunk_count);
    put64(p + kOffTotalRaw, h.total_raw_size);
    put32(p + kOffEntrySize, kChunkEntrySize);
    write_text(p + kOffModelId, kIdFieldSize, h.model_id);
    write_text(p + kOffArch, kTextFieldSize, h.architecture);
    write_text(p + kOffQuant, kTextFieldSize, h.quantization);
    std::memcpy(p + kOffManifest, h.source_manifest_hash.data(), 32);
    std::memcpy(p + kOffTableHash, h.chunk_table_hash.data(), 32);
    const Digest d = sha256(std::span<const std::uint8_t>(p, kOffHeaderHash));
    std::memcpy(p + kOffHeaderHash, d.data(), 32);
    return b;
}

void serialize_chunk_entry(const ChunkEntry& e, std::uint8_t* p) {
    put64(p, e.region_id);
    put64(p + 8, e.raw_offset);
    put64(p + 16, e.raw_size);
    put64(p + 24, e.offset);
    put64(p + 32, e.comp_size);
    put32(p + 40, static_cast<std::uint32_t>(e.codec));
    put32(p + 44, e.flags);
    std::memcpy(p + 48, e.hash.data(), 32);
}

std::vector<std::uint8_t> serialize_chunk_table(const std::vector<ChunkEntry>& entries) {
    std::vector<std::uint8_t> t(entries.size() * kChunkEntrySize);
    for (std::size_t i = 0; i < entries.size(); ++i)
        serialize_chunk_entry(entries[i], t.data() + i * kChunkEntrySize);
    return t;
}

Result<void> decode_chunk(const ChunkEntry& e, std::span<const std::uint8_t> stored,
                          std::span<std::uint8_t> raw_out, bool verify) {
    if (raw_out.size() != e.raw_size)
        return Error(ErrorCode::InvalidArgument, "output buffer size != chunk raw size");
    if (stored.size() != e.comp_size)
        return bad("stored chunk size mismatch");
    if (auto r = decompress(e.codec, stored, raw_out); !r)
        return r;
    if (verify && sha256(raw_out) != e.hash)
        return bad("chunk checksum mismatch (corrupt or tampered data)");
    return {};
}

}  // namespace hcs::storage
