// Streaming pipeline: ordering, bounded memory, cancellation, error propagation, no hangs.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "hcs/storage/model_reader.hpp"
#include "hcs/storage/writer.hpp"
#include "storage_test_util.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::storage;
namespace tst = hcs::storage::test;

namespace {
struct Fixture {
    hcs::test::TempDir td;
    std::vector<std::uint8_t> src;
    std::filesystem::path model;
    std::unique_ptr<HcsModelReader> reader;
    Fixture(Codec c, std::size_t total, std::uint32_t chunk) {
        src = tst::compressible_bytes(total / 2, 1);
        auto r = tst::random_bytes(total - src.size(), 2);
        src.insert(src.end(), r.begin(), r.end());
        tst::write_bytes(td / "s.bin", src);
        WriteOptions wo;
        wo.codec = c;
        wo.chunk_size = chunk;
        model = td / "s.hcsmodel";
        auto w = write_hcsmodel(td / "s.bin", model, wo);
        EXPECT_TRUE(w.ok());
        auto o = HcsModelReader::open(model);
        EXPECT_TRUE(o.ok());
        reader = std::move(o).value();
    }
};
}  // namespace

TEST(Stream, InOrderDeliveryAcrossBufferAndWorkerCounts) {
    for (Codec c : {Codec::Raw, Codec::Lz4, Codec::Zstd}) {
        Fixture f(c, 1'500'000, 64 * 1024);
        for (std::size_t buffers : {1u, 2u, 5u, 64u}) {
            for (std::size_t workers : {1u, 4u}) {
                StreamOptions so;
                so.buffers = buffers;
                so.workers = workers;
                std::vector<std::uint8_t> got;
                std::size_t expect_idx = 0;
                auto st = f.reader->stream(
                    [&](std::size_t idx, const ChunkEntry&, std::span<const std::uint8_t> d) -> Result<void> {
                        EXPECT_EQ(idx, expect_idx++);
                        got.insert(got.end(), d.begin(), d.end());
                        return {};
                    },
                    so);
                ASSERT_TRUE(st.ok()) << codec_name(c) << " b=" << buffers << " w=" << workers << ": " << st.error().message;
                EXPECT_EQ(got, f.src);
                EXPECT_EQ(st.value().chunks, f.reader->chunks().size());
            }
        }
    }
}

TEST(Stream, BufferMemoryIsBoundedAndBudgetEnforced) {
    Fixture f(Codec::Zstd, 4'000'000, 256 * 1024);  // ~16 chunks
    StreamOptions so;
    so.buffers = 3;
    auto st = f.reader->stream([](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> { return {}; }, so);
    ASSERT_TRUE(st.ok());
    // 3 slots * (max stored + max raw); stored <= raw + slack.
    EXPECT_LE(st.value().buffer_bytes_allocated, 3u * (256u * 1024u * 2u + 2048u));
    EXPECT_LT(st.value().buffer_bytes_allocated, f.src.size());
    so.max_buffer_bytes = 1000;  // far below the need
    auto over = f.reader->stream([](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> { return {}; }, so);
    ASSERT_FALSE(over.ok());
    EXPECT_EQ(over.error().code, ErrorCode::OutOfMemory);
    so.max_buffer_bytes = 0;
    so.buffers = 0;
    EXPECT_EQ(f.reader->stream([](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> { return {}; }, so).error().code,
              ErrorCode::InvalidArgument);
}

TEST(Stream, SlowSinkBackPressuresWithoutGrowingMemory) {
    Fixture f(Codec::Lz4, 2'000'000, 64 * 1024);
    StreamOptions so;
    so.buffers = 2;
    std::size_t n = 0;
    auto st = f.reader->stream(
        [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
            if (++n % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return {};
        },
        so);
    ASSERT_TRUE(st.ok());
    EXPECT_LE(st.value().buffer_bytes_allocated, 2u * (64u * 1024u * 2u + 1024u + 300u));
}

TEST(Stream, CancellationFromSinkStopsPromptlyAndLeaksNothing) {
    Fixture f(Codec::Zstd, 6'000'000, 64 * 1024);
    CancellationSource cs;
    StreamOptions so;
    so.buffers = 4;
    so.cancel = cs.token();
    std::size_t seen = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto st = f.reader->stream(
        [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
            if (++seen == 3) cs.request_cancel();
            return {};
        },
        so);
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, ErrorCode::Cancelled);
    EXPECT_LT(seen, f.reader->chunks().size());
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));
    // The file can be streamed again right after (no dangling reads / handles).
    std::size_t again = 0;
    auto st2 = f.reader->stream([&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> { ++again; return {}; }, StreamOptions{});
    ASSERT_TRUE(st2.ok());
    EXPECT_EQ(again, f.reader->chunks().size());
}

TEST(Stream, PreCancelledTokenReturnsCancelled) {
    Fixture f(Codec::Raw, 1'000'000, 64 * 1024);
    CancellationSource cs;
    cs.request_cancel();
    StreamOptions so;
    so.cancel = cs.token();
    auto st = f.reader->stream([](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> { return {}; }, so);
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, ErrorCode::Cancelled);
}

TEST(Stream, SinkErrorPropagatesAndStops) {
    Fixture f(Codec::Lz4, 2'000'000, 64 * 1024);
    std::size_t seen = 0;
    auto st = f.reader->stream(
        [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
            if (++seen == 2) return Error(ErrorCode::OutOfMemory, "sink full");
            return {};
        },
        StreamOptions{});
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, ErrorCode::OutOfMemory);
    EXPECT_EQ(seen, 2u);
}

TEST(Stream, SinkExceptionBecomesInternalError) {
    Fixture f(Codec::Raw, 500'000, 64 * 1024);
    auto st = f.reader->stream([](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
        throw std::runtime_error("boom");
    }, StreamOptions{});
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, ErrorCode::Internal);
}

TEST(Stream, CorruptChunkOnDiskIsDetectedWithoutHang) {
    for (Codec c : {Codec::Raw, Codec::Zstd}) {
        Fixture f(c, 1'200'000, 64 * 1024);
        auto bytes = tst::read_bytes(f.model);
        const auto& e = f.reader->chunks()[7];
        bytes[e.offset + e.comp_size / 2] ^= 0x01;
        tst::write_bytes(f.td / "bad.hcsmodel", bytes);
        auto r = HcsModelReader::open(f.td / "bad.hcsmodel");
        ASSERT_TRUE(r.ok());
        std::size_t seen = 0;
        auto st = r.value()->stream([&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> { ++seen; return {}; },
                                    StreamOptions{});
        ASSERT_FALSE(st.ok()) << codec_name(c);
        EXPECT_EQ(st.error().code, ErrorCode::ModelError);
        EXPECT_LE(seen, 7u);
    }
}

TEST(Stream, SubRangeOfChunks) {
    Fixture f(Codec::Zstd, 1'000'000, 64 * 1024);
    StreamOptions so;
    so.begin_chunk = 3;
    so.end_chunk = 6;
    std::vector<std::size_t> idx;
    std::vector<std::uint8_t> got;
    auto st = f.reader->stream(
        [&](std::size_t i, const ChunkEntry&, std::span<const std::uint8_t> d) -> Result<void> {
            idx.push_back(i);
            got.insert(got.end(), d.begin(), d.end());
            return {};
        },
        so);
    ASSERT_TRUE(st.ok());
    EXPECT_EQ(idx, (std::vector<std::size_t>{3, 4, 5}));
    EXPECT_EQ(0, std::memcmp(got.data(), f.src.data() + 3 * 64 * 1024, got.size()));
    so.begin_chunk = 9;
    so.end_chunk = 2;
    EXPECT_FALSE(f.reader->stream([](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> { return {}; }, so).ok());
}

TEST(Stream, PlainFileStreamsWithoutHashes) {
    hcs::test::TempDir td;
    const auto src = tst::random_bytes(1'234'567, 5);
    tst::write_bytes(td / "plain.bin", src);
    auto r = PlainFileReader::open(td / "plain.bin", 64 * 1024);
    ASSERT_TRUE(r.ok());
    std::vector<std::uint8_t> got;
    auto st = r.value()->stream([&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t> d) -> Result<void> {
        got.insert(got.end(), d.begin(), d.end());
        return {};
    }, StreamOptions{});
    ASSERT_TRUE(st.ok()) << st.error().message;
    EXPECT_EQ(got, src);
    std::vector<std::uint8_t> part(100);
    ASSERT_TRUE(r.value()->read_range(1000, 100, part.data()).ok());
    EXPECT_EQ(0, std::memcmp(part.data(), src.data() + 1000, 100));
    EXPECT_FALSE(r.value()->read_range(src.size() - 5, 10, part.data()).ok());
}

TEST(Stream, RepeatedStartStopIsStable) {
    Fixture f(Codec::Lz4, 800'000, 64 * 1024);
    for (int i = 0; i < 30; ++i) {
        CancellationSource cs;
        StreamOptions so;
        so.buffers = 1 + (i % 4);
        so.cancel = cs.token();
        std::size_t seen = 0;
        auto st = f.reader->stream([&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
            if (++seen == static_cast<std::size_t>(i % 5)) cs.request_cancel();
            return {};
        }, so);
        if (!st.ok()) EXPECT_EQ(st.error().code, ErrorCode::Cancelled);
    }
}
