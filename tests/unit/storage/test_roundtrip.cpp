// Writer + reader round trips on real temp files (bit-exactness, raw fallback, random access).
#include <gtest/gtest.h>

#include "hcs/storage/model_reader.hpp"
#include "hcs/storage/writer.hpp"
#include "storage_test_util.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::storage;
namespace tst = hcs::storage::test;

namespace {
// Mixed content: compressible, random, compressible.
std::vector<std::uint8_t> mixed_source(std::size_t total_len) {
    std::vector<std::uint8_t> v;
    auto a = tst::compressible_bytes(total_len / 3, 1);
    auto b = tst::random_bytes(total_len / 3, 2);
    auto c = tst::compressible_bytes(total_len - a.size() - b.size(), 3);
    v.insert(v.end(), a.begin(), a.end());
    v.insert(v.end(), b.begin(), b.end());
    v.insert(v.end(), c.begin(), c.end());
    return v;
}
}  // namespace

class RoundTrip : public ::testing::TestWithParam<Codec> {};

TEST_P(RoundTrip, BitExactViaReadRangeStreamAndEveryChunk) {
    hcs::test::TempDir td;
    const auto src = mixed_source(1'000'003);  // deliberately not a multiple of the chunk size
    const auto src_path = td / "model.bin";
    tst::write_bytes(src_path, src);
    const auto dst = td / "model.hcsmodel";

    WriteOptions wo;
    wo.codec = GetParam();
    wo.chunk_size = 64 * 1024;
    wo.model_id = "rt";
    wo.threads = 3;
    auto w = write_hcsmodel(src_path, dst, wo);
    ASSERT_TRUE(w.ok()) << w.error().message;
    EXPECT_EQ(w.value().raw_bytes, src.size());
    EXPECT_EQ(w.value().chunks, (src.size() + wo.chunk_size - 1) / wo.chunk_size);
    EXPECT_EQ(w.value().source_sha256_hex, to_hex(sha256(src)));
    EXPECT_EQ(std::filesystem::file_size(dst), w.value().container_bytes);

    auto r = HcsModelReader::open(dst);
    ASSERT_TRUE(r.ok()) << r.error().message;
    auto& rd = *r.value();
    EXPECT_EQ(rd.logical_size(), src.size());
    EXPECT_EQ(rd.header().model_id, "rt");
    EXPECT_EQ(rd.header().source_manifest_hash, sha256(src));

    // whole stream through read_range
    std::vector<std::uint8_t> all(src.size());
    ASSERT_TRUE(rd.read_range(0, all.size(), all.data()).ok());
    EXPECT_EQ(all, src);

    // arbitrary unaligned ranges spanning chunk boundaries
    for (auto [off, n] : {std::pair<std::uint64_t, std::size_t>{0, 1}, {65535, 2}, {65536, 65536}, {100000, 300000},
                          {src.size() - 1, 1}, {src.size() - 70000, 70000}}) {
        std::vector<std::uint8_t> got(n);
        ASSERT_TRUE(rd.read_range(off, n, got.data()).ok()) << off;
        EXPECT_EQ(0, std::memcmp(got.data(), src.data() + off, n)) << "off=" << off << " n=" << n;
    }
    EXPECT_FALSE(rd.read_range(src.size() - 1, 2, all.data()).ok());  // past the end

    // every chunk individually
    for (std::size_t i = 0; i < rd.chunks().size(); ++i) {
        std::vector<std::uint8_t> chunk;
        ASSERT_TRUE(rd.read_chunk(i, chunk).ok());
        const auto& e = rd.chunks()[i];
        ASSERT_EQ(chunk.size(), e.raw_size);
        EXPECT_EQ(0, std::memcmp(chunk.data(), src.data() + e.raw_offset, chunk.size()));
    }

    // pipeline stream
    std::vector<std::uint8_t> streamed;
    StreamOptions so;
    so.buffers = 3;
    auto st = rd.stream(
        [&](std::size_t idx, const ChunkEntry&, std::span<const std::uint8_t> d) -> Result<void> {
            EXPECT_EQ(idx * wo.chunk_size, streamed.size());
            streamed.insert(streamed.end(), d.begin(), d.end());
            return {};
        },
        so);
    ASSERT_TRUE(st.ok()) << st.error().message;
    EXPECT_EQ(streamed, src);
    EXPECT_EQ(st.value().raw_bytes, src.size());
}

INSTANTIATE_TEST_SUITE_P(Codecs, RoundTrip, ::testing::Values(Codec::Raw, Codec::Lz4, Codec::Zstd));

TEST(Writer, RawFallbackWhenSavingsBelowThreshold) {
    hcs::test::TempDir td;
    // Random data does not compress: every chunk must be stored Raw even though Zstd was requested.
    tst::write_bytes(td / "rnd.bin", tst::random_bytes(400'000, 9));
    WriteOptions wo;
    wo.codec = Codec::Zstd;
    wo.chunk_size = 64 * 1024;
    auto w = write_hcsmodel(td / "rnd.bin", td / "rnd.hcsmodel", wo);
    ASSERT_TRUE(w.ok()) << w.error().message;
    EXPECT_EQ(w.value().chunks_zstd, 0u);
    EXPECT_EQ(w.value().chunks_raw, w.value().chunks);
    // Highly compressible data with a threshold that cannot be met also falls back.
    tst::write_bytes(td / "cmp.bin", tst::compressible_bytes(400'000, 4));
    wo.min_savings = 0.999;
    auto w2 = write_hcsmodel(td / "cmp.bin", td / "cmp_strict.hcsmodel", wo);
    ASSERT_TRUE(w2.ok());
    EXPECT_EQ(w2.value().chunks_zstd, 0u);
    // ...and with the default threshold it compresses.
    wo.min_savings = 0.05;
    auto w3 = write_hcsmodel(td / "cmp.bin", td / "cmp.hcsmodel", wo);
    ASSERT_TRUE(w3.ok());
    EXPECT_EQ(w3.value().chunks_zstd, w3.value().chunks);
    EXPECT_LT(w3.value().container_bytes, 400'000u / 2);
}

TEST(Writer, MixedChunkCodecsInOneContainer) {
    hcs::test::TempDir td;
    std::vector<std::uint8_t> src = tst::compressible_bytes(128 * 1024, 1);
    auto rnd = tst::random_bytes(128 * 1024, 2);
    src.insert(src.end(), rnd.begin(), rnd.end());
    tst::write_bytes(td / "m.bin", src);
    WriteOptions wo;
    wo.codec = Codec::Lz4;
    wo.chunk_size = 64 * 1024;
    auto w = write_hcsmodel(td / "m.bin", td / "m.hcsmodel", wo);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.value().chunks_lz4, 2u);
    EXPECT_EQ(w.value().chunks_raw, 2u);
    auto r = HcsModelReader::open(td / "m.hcsmodel");
    ASSERT_TRUE(r.ok());
    std::vector<std::uint8_t> back(src.size());
    ASSERT_TRUE(r.value()->read_range(0, back.size(), back.data()).ok());
    EXPECT_EQ(back, src);
}

TEST(Writer, RefusesToOverwriteAndValidatesOptions) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "a.bin", tst::random_bytes(100000, 1));
    tst::write_bytes(td / "existing.hcsmodel", {1, 2, 3});
    WriteOptions wo;
    wo.chunk_size = 64 * 1024;
    auto w = write_hcsmodel(td / "a.bin", td / "existing.hcsmodel", wo);
    ASSERT_FALSE(w.ok());
    EXPECT_EQ(w.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(std::filesystem::file_size(td / "existing.hcsmodel"), 3u);  // untouched

    wo.chunk_size = 100;
    EXPECT_EQ(write_hcsmodel(td / "a.bin", td / "x.hcsmodel", wo).error().code, ErrorCode::InvalidArgument);
    wo.chunk_size = 64 * 1024;
    EXPECT_EQ(write_hcsmodel(td / "missing.bin", td / "y.hcsmodel", wo).error().code, ErrorCode::NotFound);
    EXPECT_FALSE(std::filesystem::exists(td / "y.hcsmodel"));
    EXPECT_FALSE(std::filesystem::exists(td / "x.hcsmodel"));
}

TEST(Writer, CancelledWriteRemovesOnlyItsOwnPartialFile) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "a.bin", tst::random_bytes(2'000'000, 1));
    tst::write_bytes(td / "bystander.txt", {9, 9, 9});
    CancellationSource cs;
    cs.request_cancel();
    WriteOptions wo;
    wo.chunk_size = 64 * 1024;
    wo.cancel = cs.token();
    auto w = write_hcsmodel(td / "a.bin", td / "c.hcsmodel", wo);
    ASSERT_FALSE(w.ok());
    EXPECT_EQ(w.error().code, ErrorCode::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(td / "c.hcsmodel"));
    EXPECT_TRUE(std::filesystem::exists(td / "bystander.txt"));
    EXPECT_TRUE(std::filesystem::exists(td / "a.bin"));
}

TEST(RandomAccess, OneChunkDecodesWithoutTouchingOthers) {
    hcs::test::TempDir td;
    const auto src = mixed_source(900'000);
    tst::write_bytes(td / "s.bin", src);
    WriteOptions wo;
    wo.codec = Codec::Zstd;
    wo.chunk_size = 64 * 1024;
    ASSERT_TRUE(write_hcsmodel(td / "s.bin", td / "s.hcsmodel", wo).ok());

    // Destroy the payload of chunk 0 and the last chunk on disk. A reader that decoded the whole
    // container (or verified everything on open) could not return chunk 5 correctly.
    auto bytes = tst::read_bytes(td / "s.hcsmodel");
    auto parsed = parse_container_image(bytes);
    ASSERT_TRUE(parsed.ok());
    const auto& ch = parsed.value().chunks;
    ASSERT_GT(ch.size(), 8u);
    for (std::size_t victim : {std::size_t{0}, ch.size() - 1}) {
        for (std::uint64_t i = 0; i < ch[victim].comp_size; ++i) bytes[ch[victim].offset + i] ^= 0xA5;
    }
    tst::write_bytes(td / "damaged.hcsmodel", bytes);

    auto r = HcsModelReader::open(td / "damaged.hcsmodel");
    ASSERT_TRUE(r.ok()) << "open parses header+table only";
    std::vector<std::uint8_t> chunk;
    ASSERT_TRUE(r.value()->read_chunk(5, chunk).ok());
    EXPECT_EQ(0, std::memcmp(chunk.data(), src.data() + ch[5].raw_offset, chunk.size()));
    EXPECT_FALSE(r.value()->read_chunk(0, chunk).ok());
    EXPECT_FALSE(r.value()->read_chunk(ch.size() - 1, chunk).ok());
    EXPECT_FALSE(r.value()->read_chunk(ch.size(), chunk).ok());
    // read_range inside chunk 6 works as well
    std::vector<std::uint8_t> part(1000);
    ASSERT_TRUE(r.value()->read_range(ch[6].raw_offset + 10, part.size(), part.data()).ok());
    EXPECT_EQ(0, std::memcmp(part.data(), src.data() + ch[6].raw_offset + 10, part.size()));
}

TEST(Reader, FactorySniffsFormatAndRejectsGarbage) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "plain.bin", tst::random_bytes(5000, 1));
    auto p = open_model_reader(td / "plain.bin");
    ASSERT_TRUE(p.ok()) << p.error().message;
    EXPECT_EQ(p.value()->kind(), "raw-file");
    tst::write_bytes(td / "tiny.gguf", tst::make_gguf());
    auto g = open_model_reader(td / "tiny.gguf");
    ASSERT_TRUE(g.ok()) << g.error().message;
    EXPECT_EQ(g.value()->kind(), "gguf");
    WriteOptions wo;
    wo.chunk_size = 64 * 1024;
    tst::write_bytes(td / "big.bin", tst::compressible_bytes(200000, 1));
    ASSERT_TRUE(write_hcsmodel(td / "big.bin", td / "big.hcsmodel", wo).ok());
    auto h = open_model_reader(td / "big.hcsmodel");
    ASSERT_TRUE(h.ok());
    EXPECT_EQ(h.value()->kind(), "hcsmodel");
    EXPECT_FALSE(open_model_reader(td / "nope.gguf").ok());
    // truncated HCSMODEL file
    auto full = tst::read_bytes(td / "big.hcsmodel");
    full.resize(full.size() - 20);
    tst::write_bytes(td / "cut.hcsmodel", full);
    EXPECT_FALSE(open_model_reader(td / "cut.hcsmodel").ok());
    // empty file
    tst::write_bytes(td / "empty.bin", {});
    EXPECT_FALSE(open_model_reader(td / "empty.bin").ok());
}
