// USB-disconnect simulation on the storage reader/writer (AGENTS.md 5/M5, 16.1 G, 17.6): the drive vanishes
// mid-stream or the file handle becomes invalid. Required outcome: no crash, a structured error (never a
// partial success), no partial output file left behind, handles released so the file can be deleted, and the
// layer is reusable afterwards. Complements test_source_faults.cpp (parser/stream over a cut file).
#include <gtest/gtest.h>

#include <chrono>
#include <set>

#include "hcs/storage/file_source.hpp"
#include "hcs/storage/model_reader.hpp"
#include "hcs/storage/stream.hpp"
#include "hcs/storage/writer.hpp"
#include "storage_test_util.hpp"
#include "temp_dir.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace hcs;
using namespace hcs::storage;
namespace tst = hcs::storage::test;
namespace fs = std::filesystem;

namespace {

// A drive that disappears after `limit` bytes were served. Counts failures so a test can prove the writer
// stopped asking after the first one.
class VanishingSource final : public ByteSource {
public:
    VanishingSource(const ByteSource& inner, std::uint64_t limit) : inner_(inner), limit_(limit) {}
    std::uint64_t size() const noexcept override { return inner_.size(); }
    Result<void> read_at(std::uint64_t offset, void* dst, std::size_t n) const override {
        if (served_ + n > limit_) {
            ++failures_;
            return make_error(ErrorCode::IoError, "injected read failure (device removed)");
        }
        served_ += n;
        return inner_.read_at(offset, dst, n);
    }
    int failures() const { return failures_; }

private:
    const ByteSource& inner_;
    std::uint64_t limit_;
    mutable std::uint64_t served_ = 0;
    mutable int failures_ = 0;
};

std::set<std::string> listing(const fs::path& dir) {
    std::set<std::string> out;
    for (const auto& e : fs::directory_iterator(dir)) out.insert(e.path().filename().string());
    return out;
}

WriteOptions small_chunks() {
    WriteOptions o;
    o.codec = Codec::Zstd;
    o.chunk_size = 64 * 1024;
    o.threads = 2;
    return o;
}

}  // namespace

TEST(Unplug, WriterSourceVanishesMidStreamAtEveryChunkBoundaryLeavesNoPartialFile) {
    hcs::test::TempDir td;
    const auto data = tst::random_bytes(8 * 64 * 1024 + 123, 11);
    MemorySource mem(data);
    const auto before = listing(td.path());
    // Source dies before, exactly at, and just past several chunk boundaries (batches of 2 threads => 128
    // KiB).
    for (std::uint64_t limit :
         {0ull, 1ull, 65535ull, 65536ull, 65537ull, 131072ull, 200000ull, 400000ull, data.size() - 1}) {
        VanishingSource bad(mem, limit);
        Result<WriteStats> r = Error(ErrorCode::Internal, "unset");
        ASSERT_NO_THROW(r = write_hcsmodel(bad, "vanish", td / "out.hcsmodel", small_chunks())) << limit;
        ASSERT_FALSE(r.ok()) << "a vanished source cannot produce a successful write (limit " << limit << ")";
        EXPECT_EQ(r.error().code, ErrorCode::IoError) << limit;
        EXPECT_EQ(bad.failures(), 1) << "the writer kept reading after the device failed (limit " << limit
                                     << ")";
        EXPECT_FALSE(fs::exists(td / "out.hcsmodel")) << "partial output left behind (limit " << limit << ")";
        EXPECT_EQ(listing(td.path()), before) << "stray file after failure (limit " << limit << ")";
    }
}

TEST(Unplug, WriterIsReusableAfterAFailureAndProducesAValidContainer) {
    hcs::test::TempDir td;
    const auto data = tst::compressible_bytes(300'000, 12);
    MemorySource mem(data);
    VanishingSource bad(mem, 100'000);
    ASSERT_FALSE(write_hcsmodel(bad, "retry", td / "out.hcsmodel", small_chunks()).ok());
    ASSERT_FALSE(fs::exists(td / "out.hcsmodel"));
    // The same destination name is free again (no leaked handle, no leftover file) and the retry round-trips.
    auto ok = write_hcsmodel(mem, "retry", td / "out.hcsmodel", small_chunks());
    ASSERT_TRUE(ok.ok()) << ok.error().message;
    auto reader = HcsModelReader::open(td / "out.hcsmodel");
    ASSERT_TRUE(reader.ok()) << reader.error().message;
    std::vector<std::uint8_t> back(data.size());
    ASSERT_TRUE(reader.value()->read_range(0, back.size(), back.data()).ok());
    EXPECT_EQ(back, data);
}

TEST(Unplug, WriterFromARealFileThatIsCutShortFailsAndLeavesNothing) {
    hcs::test::TempDir td;
    // A header-valid chunk table is irrelevant here: the writer reads the *source* sequentially. We emulate
    // the drive dropping out by giving it a source whose reported size is larger than what the file really
    // holds (a file system that lost the tail): FileSource refuses the read past the end instead of
    // short-succeeding.
    const auto data = tst::random_bytes(300'000, 13);
    tst::write_bytes(td / "src.bin", data);
    auto file = FileSource::open(td / "src.bin", true);
    ASSERT_TRUE(file.ok());
    class Lying final : public ByteSource {
    public:
        explicit Lying(const ByteSource& in) : in_(in) {}
        std::uint64_t size() const noexcept override { return in_.size() + 200'000; }  // claims a longer file
        Result<void> read_at(std::uint64_t o, void* d, std::size_t n) const override {
            return in_.read_at(o, d, n);
        }

    private:
        const ByteSource& in_;
    } lying(*file.value());
    auto r = write_hcsmodel(lying, "lying", td / "out.hcsmodel", small_chunks());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
    EXPECT_FALSE(fs::exists(td / "out.hcsmodel"));
    EXPECT_EQ(listing(td.path()), std::set<std::string>{"src.bin"});
}

TEST(Unplug, FailedStreamReleasesTheFileHandleSoTheFileCanBeDeleted) {
    hcs::test::TempDir td;
    auto src = tst::random_bytes(500'000, 14);
    tst::write_bytes(td / "s.bin", src);
    WriteOptions wo = small_chunks();
    ASSERT_TRUE(write_hcsmodel(td / "s.bin", td / "s.hcsmodel", wo).ok());
    auto whole = tst::read_bytes(td / "s.hcsmodel");
    whole.resize(whole.size() / 2);  // the tail of the drive is gone
    tst::write_bytes(td / "cut.hcsmodel", whole);
    {
        // Open against the intact table, stream over the cut file: the pipeline fails mid-way.
        auto good = HcsModelReader::open(td / "s.hcsmodel");
        ASSERT_TRUE(good.ok());
        auto cut = FileSource::open(td / "cut.hcsmodel");
        ASSERT_TRUE(cut.ok());
        std::size_t delivered = 0;
        const auto t0 = std::chrono::steady_clock::now();
        auto st =
            stream_chunks(*cut.value(), good.value()->chunks(), StreamOptions{},
                          [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
                              ++delivered;
                              return {};
                          });
        ASSERT_FALSE(st.ok());
        EXPECT_LT(delivered, good.value()->chunks().size());
        EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));
    }
    // Windows keeps a file undeletable while any handle is open (FileSource opens with FILE_SHARE_READ only),
    // so a successful delete proves the failed pipeline leaked no handle.
    std::error_code ec;
    EXPECT_TRUE(fs::remove(td / "cut.hcsmodel", ec)) << ec.message();
    EXPECT_TRUE(fs::remove(td / "s.hcsmodel", ec)) << ec.message();
}

#ifdef _WIN32
// The handle itself becomes invalid (device surprise-removal, driver reset). The test closes the OS handle
// behind the FileSource's back; the FileSource object is then intentionally leaked (its destructor would
// close the same numeric handle a second time, which is exactly the misuse being simulated, not a product
// path).
TEST(Unplug, InvalidFileHandleIsAStructuredIoErrorNotACrash) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "f.bin", tst::random_bytes(200'000, 15));
    auto opened = FileSource::open(td / "f.bin");
    ASSERT_TRUE(opened.ok());
    FileSource* fsrc = opened.value().release();  // leaked on purpose, see above
    std::vector<std::uint8_t> buf(4096);
    ASSERT_TRUE(fsrc->read_at(0, buf.data(), buf.size()).ok());
    ASSERT_TRUE(CloseHandle(static_cast<HANDLE>(fsrc->native_handle())));
    auto r = fsrc->read_at(0, buf.data(), buf.size());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
    EXPECT_NE(r.error().message.find("Win32 error"), std::string::npos) << r.error().message;
}

// The random-access handle (read_range / read_chunk) is invalid. The streaming pipeline opens its OWN
// overlapped handle on the same path (stream.cpp), so it is independent of the yanked one - the test pins
// both facts: the random-access path fails with a structured error, the streaming path still verifies every
// chunk.
TEST(Unplug, InvalidRandomAccessHandleFailsCleanlyWhileStreamingKeepsItsOwnHandle) {
    hcs::test::TempDir td;
    auto src = tst::random_bytes(400'000, 16);
    tst::write_bytes(td / "s.bin", src);
    ASSERT_TRUE(write_hcsmodel(td / "s.bin", td / "s.hcsmodel", small_chunks()).ok());
    auto reader = HcsModelReader::open(td / "s.hcsmodel");
    ASSERT_TRUE(reader.ok());
    HcsModelReader* rd = reader.value().release();  // leaked on purpose (see the test above)
    ASSERT_TRUE(CloseHandle(static_cast<HANDLE>(rd->file().native_handle())));
    std::vector<std::uint8_t> chunk;
    auto rc = rd->read_chunk(0, chunk);
    ASSERT_FALSE(rc.ok());
    EXPECT_EQ(rc.error().code, ErrorCode::IoError) << rc.error().message;
    std::vector<std::uint8_t> back(1024);
    auto rr = rd->read_range(0, back.size(), back.data());
    ASSERT_FALSE(rr.ok());
    EXPECT_EQ(rr.error().code, ErrorCode::IoError);
    std::size_t delivered = 0;
    auto st = rd->stream(
        [&](std::size_t, const ChunkEntry&, std::span<const std::uint8_t>) -> Result<void> {
            ++delivered;
            return {};
        },
        StreamOptions{});
    ASSERT_TRUE(st.ok()) << st.error().message;
    EXPECT_EQ(delivered, rd->chunks().size());
}
#endif
