// atomic_write_file / read_file_limited (state files: settings, registry, caches).
#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>

#include "hcs/fs_util.hpp"
#include "temp_dir.hpp"

using namespace hcs;
namespace fs = std::filesystem;

namespace {
std::size_t count_tmp_leftovers(const fs::path& dir) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().filename().string().find(".tmp.") != std::string::npos)
            ++n;
    return n;
}
}  // namespace

TEST(FsUtil, AtomicWriteCreatesParentsAndRoundTripsBinary) {
    test::TempDir td;
    const fs::path p = td / "a" / "b" / "c.json";
    const std::string bin("line1\r\nline2\0\xff\x01", 17);
    ASSERT_TRUE(atomic_write_file(p, bin).ok());
    auto r = read_file_limited(p, 1024);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), bin);
    EXPECT_EQ(count_tmp_leftovers(p.parent_path()), 0u);
}

TEST(FsUtil, AtomicWriteReplacesExistingAndEmptyContentIsAllowed) {
    test::TempDir td;
    const fs::path p = td / "x.txt";
    ASSERT_TRUE(atomic_write_file(p, "first").ok());
    ASSERT_TRUE(atomic_write_file(p, "second-longer").ok());
    EXPECT_EQ(read_file_limited(p, 100).value(), "second-longer");
    ASSERT_TRUE(atomic_write_file(p, "").ok());
    EXPECT_EQ(read_file_limited(p, 100).value(), "");
    EXPECT_EQ(count_tmp_leftovers(td.path()), 0u);
}

TEST(FsUtil, FailedRenameLeavesPreviousStateAndNoTempFile) {
    test::TempDir td;
    // The destination is a non-empty directory: the rename cannot succeed.
    const fs::path p = td / "dest";
    fs::create_directories(p);
    test::write_file(p / "keep.txt", "precious");
    auto r = atomic_write_file(p, "new content");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
    EXPECT_TRUE(fs::is_directory(p));
    EXPECT_EQ(read_file_limited(p / "keep.txt", 100).value(), "precious");
    EXPECT_EQ(count_tmp_leftovers(td.path()), 0u) << "temp file leaked after a failed replace";
}

TEST(FsUtil, ParentThatIsAFileIsAnErrorNotACrash) {
    test::TempDir td;
    test::write_file(td / "plain", "i am a file");
    auto r = atomic_write_file(td / "plain" / "child.json", "x");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::IoError);
    EXPECT_EQ(read_file_limited(td / "plain", 100).value(), "i am a file");
}

TEST(FsUtil, ConcurrentWritersNeverProduceMixedContent) {
    test::TempDir td;
    const fs::path p = td / "shared.json";
    std::set<std::string> valid;
    constexpr int kWriters = 6, kRounds = 40;
    for (int w = 0; w < kWriters; ++w)
        valid.insert(std::string(20000 + w * 1000, static_cast<char>('a' + w)));
    ASSERT_TRUE(atomic_write_file(p, *valid.begin()).ok());
    std::atomic<bool> stop{false};
    std::atomic<int> mixed{0}, write_ok{0};
    std::thread reader([&] {
        while (!stop.load()) {
            auto r = read_file_limited(p, 1 << 20);  // may fail while a rename is in flight; never mixed
            if (r.ok() && !valid.count(r.value()))
                ++mixed;
        }
    });
    std::vector<std::thread> ws;
    for (int w = 0; w < kWriters; ++w)
        ws.emplace_back([&, w] {
            const std::string mine(20000 + w * 1000, static_cast<char>('a' + w));
            for (int i = 0; i < kRounds; ++i)
                if (atomic_write_file(p, mine).ok())
                    ++write_ok;
        });
    for (auto& t : ws) t.join();
    stop = true;
    reader.join();
    EXPECT_EQ(mixed.load(), 0) << "a reader saw a torn/mixed file";
    EXPECT_GT(write_ok.load(), 0);
    auto fin = read_file_limited(p, 1 << 20);
    ASSERT_TRUE(fin.ok());
    EXPECT_TRUE(valid.count(fin.value())) << "final content is not exactly one writer's payload";
    EXPECT_EQ(count_tmp_leftovers(td.path()), 0u);
}

TEST(FsUtil, ReadFileLimitedEnforcesTheBoundBeforeAllocating) {
    test::TempDir td;
    test::write_file(td / "f", std::string(1000, 'z'));
    EXPECT_TRUE(read_file_limited(td / "f", 1000).ok());
    auto big = read_file_limited(td / "f", 999);
    ASSERT_FALSE(big.ok());
    EXPECT_EQ(big.error().code, ErrorCode::InvalidArgument);
    auto missing = read_file_limited(td / "nope", 10);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    auto dir = read_file_limited(td.path(), 1 << 20);  // a directory is not a readable file
    EXPECT_FALSE(dir.ok());
}
