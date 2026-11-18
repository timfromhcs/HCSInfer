// Read-failure / unplug paths of the byte-source layer: an injected failing ByteSource and a file cut short
// under a valid chunk table. Every failure must surface as an error (never a partial success), without
// crash, hang or leaked workers, and the layer must be reusable right afterwards.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>

#include "hcs/storage/file_source.hpp"
#include "hcs/storage/gguf.hpp"
#include "hcs/storage/model_reader.hpp"
#include "hcs/storage/writer.hpp"
#include "storage_test_util.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::storage;
namespace tst = hcs::storage::test;

namespace {

// Behaves like a USB drive that vanishes after `limit` bytes were served (or fails every read when
// `fail_all`). Counts reads so tests can prove the parser stopped asking after the first failure.
class FailingSource final : public ByteSource {
public:
    FailingSource(const ByteSource& inner, std::uint64_t limit, ErrorCode code = ErrorCode::IoError)
        : inner_(inner), limit_(limit), code_(code) {}
    std::uint64_t size() const noexcept override { return inner_.size(); }
    Result<void> read_at(std::uint64_t offset, void* dst, std::size_t n) const override {
        ++reads_;
        if (served_ + n > limit_) {
            ++failures_;
            return make_error(code_, "injected read failure (device removed)");
        }
        served_ += n;
        return inner_.read_at(offset, dst, n);
    }
    int reads() const { return reads_; }
    int failures() const { return failures_; }

private:
    const ByteSource& inner_;
    std::uint64_t limit_;
    ErrorCode code_;
    mutable std::uint64_t served_ = 0;
    mutable int reads_ = 0;
    mutable int failures_ = 0;
};

}  // namespace

TEST(SourceFaults, GgufParseFailsCleanlyForEveryInjectionPoint) {
    const auto good = tst::make_gguf();
    MemorySource mem(good);
    ASSERT_TRUE(parse_gguf(mem).ok());
    for (std::uint64_t limit = 0; limit < good.size(); limit += 7) {
        FailingSource bad(mem, limit);
        Result<GgufInfo> r = Error(ErrorCode::Internal, "unset");
        ASSERT_NO_THROW(r = parse_gguf(bad)) << "limit=" << limit;
        ASSERT_FALSE(r.ok()) << "parse cannot succeed when reads died at byte " << limit;
        EXPECT_TRUE(r.error().code == ErrorCode::IoError || r.error().code == ErrorCode::ModelError)
            << "limit=" << limit << " code=" << to_string(r.error().code);
        EXPECT_LE(bad.failures(), 1) << "the parser kept reading after the device failed (limit=" << limit
                                     << ")";
    }
}

TEST(SourceFaults, ReadErrorClassIsPropagatedNotMasked) {
    const auto good = tst::make_gguf();
    MemorySource mem(good);
    FailingSource bad(mem, 0, ErrorCode::IoError);
    auto r = parse_gguf(bad);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
}

TEST(SourceFaults, MemorySourceRejectsOutOfRangeAndOverflowingReads) {
    std::vector<std::uint8_t> b(16, 1);
    MemorySource m(b);
    std::uint8_t out[32] = {};
    EXPECT_TRUE(m.read_at(0, out, 16).ok());
    EXPECT_FALSE(m.read_at(1, out, 16).ok());
    EXPECT_FALSE(m.read_at(17, out, 1).ok());
    EXPECT_FALSE(m.read_at(~0ull, out, 16).ok()) << "offset + size overflow must be rejected, not wrapped";
    EXPECT_TRUE(m.read_at(16, out, 0).ok());
}

TEST(SourceFaults, FileSourceReadBeyondEndIsAnErrorNeverAShortSuccess) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "f.bin", tst::random_bytes(4096, 3));
    auto f = FileSource::open(td / "f.bin");
    ASSERT_TRUE(f.ok());
    std::vector<std::uint8_t> buf(8192);
    EXPECT_TRUE(f.value()->read_at(0, buf.data(), 4096).ok());
    EXPECT_FALSE(f.value()->read_at(0, buf.data(), 4097).ok());
    EXPECT_FALSE(f.value()->read_at(4000, buf.data(), 200).ok());
    EXPECT_FALSE(f.value()->read_at(~0ull - 10, buf.data(), 100).ok());
    // Still usable after the failures.
    EXPECT_TRUE(f.value()->read_at(100, buf.data(), 50).ok());
}

TEST(SourceFaults, MissingFileIsAnErrorNotACrash) {
    hcs::test::TempDir td;
    auto f = FileSource::open(td / "does-not-exist.bin");
    ASSERT_FALSE(f.ok());
    EXPECT_FALSE(f.error().message.empty());
}

// The drive vanished mid-copy: the chunk table (read earlier) is valid, the payload is cut short. The
// streaming pipeline must report the failure, stop, and be reusable (no dangling reads/handles).
TEST(SourceFaults, StreamOverFileCutShortFailsWithoutHangAndRecovers) {
    hcs::test::TempDir td;
    auto src = tst::compressible_bytes(600'000, 5);
    auto rnd = tst::random_bytes(600'000, 6);
    src.insert(src.end(), rnd.begin(), rnd.end());
    tst::write_bytes(td / "s.bin", src);
    WriteOptions wo;
    wo.codec = Codec::Zstd;
    wo.chunk_size = 64 * 1024;
    ASSERT_TRUE(write_hcsmodel(td / "s.bin", td / "s.hcsmodel", wo).ok());
    auto reader = HcsModelReader::open(td / "s.hcsmodel");
    ASSERT_TRUE(reader.ok());
    const auto& chunks = reader.value()->chunks();
    ASSERT_GT(chunks.size(), 8u);
    auto whole = tst::read_bytes(td / "s.hcsmodel");
    for (double frac : {0.15, 0.5, 0.9}) {
        auto cut = whole;
        cut.resize(static_cast<std::size_t>(static_cast<double>(whole.size()) * frac));
        tst::write_bytes(td / "cut.bin", cut);
        auto fs = FileSource::open(td / "cut.bin");
        ASSERT_TRUE(fs.ok());
        std::size_t delivered = 0;
        const auto t0 = std::chrono::steady_clock::now();
        auto st =
            stream_chunks(*fs.value(), chunks, StreamOptions{},
                          [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
                              ++delivered;
                              return {};
                          });
        ASSERT_FALSE(st.ok()) << "a truncated source must not stream to completion (frac=" << frac << ")";
        EXPECT_TRUE(st.error().code == ErrorCode::IoError || st.error().code == ErrorCode::ModelError)
            << to_string(st.error().code);
        EXPECT_LT(delivered, chunks.size());
        EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10)) << "hang on read failure";
    }
    // The intact file still streams completely after all the failures.
    std::size_t again = 0;
    auto st2 = reader.value()->stream(
        [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
            ++again;
            return {};
        },
        StreamOptions{});
    ASSERT_TRUE(st2.ok());
    EXPECT_EQ(again, chunks.size());
}
