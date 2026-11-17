// Adversarial tests for the HCSMODEL parser (AGENTS.md 16.1 C): every malformed container must be
// rejected with an error, never crash, over-read or allocate based on an unvalidated field.
#include <gtest/gtest.h>

#include <limits>

#include "hcs/checked_math.hpp"
#include "hcs/storage/hcsmodel.hpp"
#include "storage_test_util.hpp"

using namespace hcs;
using namespace hcs::storage;
using test::assemble;
using test::make_parts;
using test::Parts;

namespace {
Result<ParsedContainer> parse(const std::vector<std::uint8_t>& img, bool verify = true) {
    return parse_container_image(img, ParseOptions{verify});
}
void expect_rejected(const std::vector<std::uint8_t>& img, ErrorCode code, const char* why) {
    auto r = parse(img);
    ASSERT_FALSE(r.ok()) << why;
    EXPECT_EQ(r.error().code, code) << why << ": " << r.error().message;
}
}  // namespace

TEST(HcsModelParser, AcceptsValidContainerAndExposesFields) {
    auto p = make_parts(5, 4096, Codec::Lz4);
    auto img = assemble(p);
    auto r = parse(img);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().header.chunk_count, 5u);
    EXPECT_EQ(r.value().header.total_raw_size, 5u * 4096u);
    EXPECT_EQ(r.value().header.model_id, "unit-test");
    EXPECT_EQ(r.value().header.architecture, "test");
    ASSERT_EQ(r.value().chunks.size(), 5u);
    EXPECT_EQ(r.value().chunks[3].raw_offset, 3u * 4096u);
}

TEST(HcsModelParser, TruncatedHeader) {
    auto img = assemble(make_parts(3, 4096, Codec::Raw));
    for (std::size_t n : {std::size_t{0}, std::size_t{7}, std::size_t{100}, std::size_t{kHeaderSize - 1}}) {
        std::vector<std::uint8_t> cut(img.begin(), img.begin() + static_cast<std::ptrdiff_t>(n));
        expect_rejected(cut, ErrorCode::ModelError, "truncated header");
    }
}

TEST(HcsModelParser, TruncatedTableAndTruncatedPayload) {
    auto img = assemble(make_parts(4, 4096, Codec::Raw));
    // Cut inside the chunk table (table is last in the file).
    std::vector<std::uint8_t> cut_table(img.begin(), img.end() - 10);
    expect_rejected(cut_table, ErrorCode::ModelError, "truncated table");
    // Table first, payload cut: place the table right after the header and truncate the payload.
    auto p = make_parts(4, 4096, Codec::Raw);
    const auto table = serialize_chunk_table(p.e);
    // Rebuild with table at offset 288 and payload after it.
    const std::uint64_t shift = table.size();
    for (auto& e : p.e) e.offset += shift;
    p.h.chunk_table_offset = kHeaderSize;
    auto table2 = serialize_chunk_table(p.e);
    p.h.chunk_table_hash = sha256(table2);
    auto full = serialize_header(p.h);
    full.insert(full.end(), table2.begin(), table2.end());
    full.insert(full.end(), p.payload.begin(), p.payload.end());
    ASSERT_TRUE(parse(full).ok()) << "table-first layout is valid";
    std::vector<std::uint8_t> cut_payload(full.begin(), full.end() - 100);
    expect_rejected(cut_payload, ErrorCode::ModelError, "truncated payload");
}

TEST(HcsModelParser, BadMagicVersionEndianFlagsReserved) {
    auto good = assemble(make_parts(2, 4096, Codec::Raw));
    auto img = good;
    img[0] = 'X';
    expect_rejected(img, ErrorCode::ModelError, "magic");

    Parts p = make_parts(2, 4096, Codec::Raw);
    p.h.format_version = 2;
    expect_rejected(assemble(p), ErrorCode::Unsupported, "version 2");

    img = good;
    img[16] = 0x01;  // endian marker byte order flipped
    img[19] = 0x04;
    expect_rejected(img, ErrorCode::ModelError, "endianness marker");

    img = good;
    img[20] = 1;  // header flags
    expect_rejected(img, ErrorCode::Unsupported, "unknown header flags");

    img = good;
    img[28] = 1;  // reserved
    expect_rejected(img, ErrorCode::ModelError, "reserved nonzero");

    img = good;
    img[12] = 200;  // header size field
    expect_rejected(img, ErrorCode::ModelError, "header size");
}

TEST(HcsModelParser, HeaderAndTableChecksumMismatch) {
    auto good = assemble(make_parts(3, 4096, Codec::Raw));
    auto img = good;
    img[100] ^= 1;  // inside the architecture text -> header hash mismatch
    expect_rejected(img, ErrorCode::ModelError, "header hash");
    // The same corruption passes when checksum verification is disabled (fuzz mode) yet stays memory-safe.
    (void)parse(img, false);

    img = good;
    img[img.size() - 5] ^= 1;  // inside the table -> table hash mismatch
    expect_rejected(img, ErrorCode::ModelError, "table hash");
}

TEST(HcsModelParser, UnknownCodecAndFlags) {
    Parts p = make_parts(3, 4096, Codec::Raw);
    p.e[1].codec = static_cast<Codec>(7);
    expect_rejected(assemble(p), ErrorCode::Unsupported, "unknown codec");
    p = make_parts(3, 4096, Codec::Raw);
    p.e[2].flags = 0x80;
    expect_rejected(assemble(p), ErrorCode::Unsupported, "unknown chunk flags");
}

TEST(HcsModelParser, OffsetPlusSizeOverflow) {
    Parts p = make_parts(3, 4096, Codec::Raw);
    p.e[1].offset = std::numeric_limits<std::uint64_t>::max() - 10;  // offset + 4096 wraps
    expect_rejected(assemble(p), ErrorCode::ModelError, "offset+size overflow");
    p = make_parts(3, 4096, Codec::Raw);
    p.e[1].comp_size = std::numeric_limits<std::uint64_t>::max();
    expect_rejected(assemble(p), ErrorCode::ModelError, "huge comp size");
    p = make_parts(3, 4096, Codec::Raw);
    p.h.chunk_table_offset = std::numeric_limits<std::uint64_t>::max() - 5;
    expect_rejected(assemble(p, false), ErrorCode::ModelError, "table offset overflow");
}

TEST(HcsModelParser, OverlappingChunkPayloads) {
    Parts p = make_parts(4, 4096, Codec::Raw);
    p.e[2].offset = p.e[1].offset + 100;  // overlaps chunk 1 (and still inside the file)
    expect_rejected(assemble(p), ErrorCode::ModelError, "overlapping chunks");
    p = make_parts(4, 4096, Codec::Raw);
    p.e[3].offset = p.e[0].offset;  // identical region
    expect_rejected(assemble(p), ErrorCode::ModelError, "identical chunk regions");
}

TEST(HcsModelParser, ChunkOverlapsTableOrHeader) {
    Parts p = make_parts(3, 4096, Codec::Raw);
    p.e[0].offset = 16;  // inside the header
    expect_rejected(assemble(p), ErrorCode::ModelError, "chunk inside header");
    p = make_parts(3, 4096, Codec::Raw);
    // Point the last chunk into the table region.
    p.e[2].offset = kHeaderSize + p.payload.size() + 8;
    expect_rejected(assemble(p), ErrorCode::ModelError, "chunk overlapping table / beyond file");
}

TEST(HcsModelParser, HugeDeclaredAllocationsAreCapped) {
    Parts p = make_parts(2, 4096, Codec::Raw);
    p.h.chunk_count = 1ull << 40;  // would need a 80 TiB table
    expect_rejected(assemble(p), ErrorCode::ModelError, "huge chunk count");
    p = make_parts(2, 4096, Codec::Raw);
    p.e[0].raw_size = 1ull << 40;
    expect_rejected(assemble(p), ErrorCode::ModelError, "huge raw size");
    p = make_parts(2, 4096, Codec::Raw);
    p.h.total_raw_size = std::numeric_limits<std::uint64_t>::max();
    expect_rejected(assemble(p), ErrorCode::ModelError, "huge total size");
    p = make_parts(2, 4096, Codec::Raw);
    p.h.nominal_chunk_size = 0xFFFFFFFFu;
    expect_rejected(assemble(p), ErrorCode::ModelError, "nominal chunk size above cap");
    // A table that claims more entries than the file can hold is rejected before any allocation.
    p = make_parts(2, 4096, Codec::Raw);
    p.h.chunk_count = 1000000;
    expect_rejected(assemble(p), ErrorCode::ModelError, "table larger than file");
}

TEST(HcsModelParser, NonContiguousOrInconsistentSizes) {
    Parts p = make_parts(3, 4096, Codec::Raw);
    p.e[1].raw_offset += 1;
    expect_rejected(assemble(p), ErrorCode::ModelError, "gap in logical stream");
    p = make_parts(3, 4096, Codec::Raw);
    p.h.total_raw_size += 1;
    expect_rejected(assemble(p), ErrorCode::ModelError, "total mismatch");
    p = make_parts(3, 4096, Codec::Raw);
    p.e[0].raw_size = 0;
    expect_rejected(assemble(p), ErrorCode::ModelError, "zero-size chunk");
    p = make_parts(3, 4096, Codec::Raw);
    p.e[1].comp_size = 4095;  // Raw chunk must have comp == raw
    expect_rejected(assemble(p), ErrorCode::ModelError, "raw size mismatch");
    p = make_parts(3, 4096, Codec::Zstd);
    p.e[1].comp_size = 4096 * 2;  // implausibly large stored size for 4096 raw bytes
    expect_rejected(assemble(p), ErrorCode::ModelError, "stored size beyond bound");
}

TEST(HcsModelParser, BadTextFields) {
    Parts p = make_parts(2, 4096, Codec::Raw);
    p.h.model_id = std::string("ab") + "cd";
    expect_rejected(assemble(p), ErrorCode::ModelError, "control char in id");
    auto img = assemble(make_parts(2, 4096, Codec::Raw));
    // data after NUL terminator in the quantization field (re-hash header so only this is wrong)
    img[160 + 5] = 'x';
    const auto h = sha256(std::span<const std::uint8_t>(img.data(), 256));
    std::copy(h.begin(), h.end(), img.begin() + 256);
    expect_rejected(img, ErrorCode::ModelError, "data after terminator");
}

TEST(HcsModelParser, WrongChunkHashDetectedAtDecode) {
    Parts p = make_parts(3, 4096, Codec::Raw);
    p.e[1].hash[0] ^= 0xFF;
    auto img = assemble(p);
    auto parsed = parse(img);
    ASSERT_TRUE(parsed.ok()) << "hash is only checked when data is decoded";
    const auto& e = parsed.value().chunks[1];
    std::vector<std::uint8_t> out(e.raw_size);
    auto r = decode_chunk(e, std::span<const std::uint8_t>(img.data() + e.offset, e.comp_size), out, true);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::ModelError);
    EXPECT_TRUE(
        decode_chunk(e, std::span<const std::uint8_t>(img.data() + e.offset, e.comp_size), out, false).ok());
}

TEST(HcsModelParser, CorruptCompressedBlockDetectedAtDecode) {
    for (Codec c : {Codec::Lz4, Codec::Zstd}) {
        Parts p = make_parts(2, 65536, c);
        auto img = assemble(p);
        auto parsed = parse(img);
        ASSERT_TRUE(parsed.ok());
        const auto& e = parsed.value().chunks[0];
        ASSERT_EQ(e.codec, c);
        ASSERT_LT(e.comp_size, e.raw_size);
        std::vector<std::uint8_t> stored(img.begin() + static_cast<std::ptrdiff_t>(e.offset),
                                         img.begin() + static_cast<std::ptrdiff_t>(e.offset + e.comp_size));
        std::vector<std::uint8_t> out(e.raw_size);
        ASSERT_TRUE(decode_chunk(e, stored, out, true).ok());
        for (std::size_t pos = 0; pos < stored.size(); pos += stored.size() / 40 + 1) {
            auto bad = stored;
            bad[pos] ^= 0x55;
            EXPECT_FALSE(decode_chunk(e, bad, out, true).ok())
                << codec_name(c) << " pos=" << pos;  // hash always catches it
        }
    }
}

TEST(HcsModelParser, SerializationRoundTrip) {
    Parts p = make_parts(7, 8192, Codec::Zstd);
    auto parsed = parse(assemble(p));
    ASSERT_TRUE(parsed.ok());
    const auto& e = parsed.value().chunks;
    ASSERT_EQ(e.size(), p.e.size());
    for (std::size_t i = 0; i < e.size(); ++i) {
        EXPECT_EQ(e[i].offset, p.e[i].offset);
        EXPECT_EQ(e[i].comp_size, p.e[i].comp_size);
        EXPECT_EQ(e[i].hash, p.e[i].hash);
        EXPECT_EQ(e[i].codec, p.e[i].codec);
    }
    EXPECT_EQ(parsed.value().header.source_manifest_hash, p.h.source_manifest_hash);
}

TEST(CheckedMath, StorageRelevantBoundaries) {
    constexpr auto M = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(checked_add(M, 1).has_value());
    EXPECT_FALSE(checked_mul(M, 2).has_value());
    EXPECT_TRUE(checked_mul(0, M).has_value());
    EXPECT_FALSE(range_within(M - 1, 2, M));
    EXPECT_TRUE(range_within(10, 5, 15));
    EXPECT_FALSE(range_within(10, 6, 15));
}
