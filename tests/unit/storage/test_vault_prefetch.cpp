// Vault load-path stage: sequential bounded read + whole-file SHA-256 before the engine opens the file.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>

#include "hcs/job_queue.hpp"
#include "hcs/storage/sha256.hpp"
#include "hcs/storage/vault_prefetch.hpp"
#include "storage_test_util.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::storage;
namespace tst = hcs::storage::test;

namespace {
constexpr std::uint32_t kMiB = 1u << 20;

std::set<std::string> listing(const std::filesystem::path& dir) {
    std::set<std::string> s;
    for (const auto& e : std::filesystem::directory_iterator(dir)) s.insert(e.path().filename().string());
    return s;
}
}  // namespace

TEST(VaultPrefetch, DigestEqualsIndependentSha256OverManyChunks) {
    hcs::test::TempDir td;
    auto data = tst::random_bytes(3 * kMiB + 12345, 7);  // 4 chunks, the last one short
    tst::write_bytes(td / "m.gguf", data);
    PrefetchOptions o;
    o.chunk_bytes = kMiB;
    o.buffers = 2;
    auto r = prefetch_and_verify(td / "m.gguf", o);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().bytes, data.size());
    EXPECT_EQ(r.value().sha256, to_hex(sha256(data)));
    EXPECT_FALSE(r.value().verified);  // nothing was expected
}

TEST(VaultPrefetch, ExpectedDigestMatchIsVerified) {
    hcs::test::TempDir td;
    auto data = tst::random_bytes(2 * kMiB + 1, 8);
    tst::write_bytes(td / "m.gguf", data);
    PrefetchOptions o;
    o.chunk_bytes = kMiB;
    o.expected_sha256 = to_hex(sha256(data));
    auto r = prefetch_and_verify(td / "m.gguf", o);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_TRUE(r.value().verified);
}

TEST(VaultPrefetch, SingleFlippedBitIsRejectedAsModelError) {
    hcs::test::TempDir td;
    auto data = tst::random_bytes(2 * kMiB + 1, 9);
    const std::string good = to_hex(sha256(data));
    data[kMiB + 17] ^= 0x01;  // corruption in the second chunk
    tst::write_bytes(td / "m.gguf", data);
    PrefetchOptions o;
    o.chunk_bytes = kMiB;
    o.expected_sha256 = good;
    auto r = prefetch_and_verify(td / "m.gguf", o);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::ModelError);
    EXPECT_NE(r.error().message.find("mismatch"), std::string::npos);
}

TEST(VaultPrefetch, BufferMemoryIsFixedAndIndependentOfFileSize) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "small.bin", tst::random_bytes(2 * kMiB, 1));
    tst::write_bytes(td / "big.bin", tst::random_bytes(9 * kMiB, 2));
    PrefetchOptions o;
    o.chunk_bytes = kMiB;
    o.buffers = 2;
    auto a = prefetch_and_verify(td / "small.bin", o);
    auto b = prefetch_and_verify(td / "big.bin", o);
    ASSERT_TRUE(a.ok() && b.ok());
    EXPECT_LE(b.value().buffer_bytes, std::uint64_t{o.buffers} * o.chunk_bytes * 2);
    EXPECT_EQ(a.value().buffer_bytes, b.value().buffer_bytes);
}

TEST(VaultPrefetch, WritesNothingNextToTheModelOrAnywhereInItsDirectory) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "m.gguf", tst::random_bytes(3 * kMiB, 3));
    const auto before = listing(td.path());
    PrefetchOptions o;
    o.chunk_bytes = kMiB;
    ASSERT_TRUE(prefetch_and_verify(td / "m.gguf", o).ok());
    EXPECT_EQ(listing(td.path()), before);
}

TEST(VaultPrefetch, RejectsBadOptionsMissingAndEmptyFiles) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "m.gguf", tst::random_bytes(kMiB, 4));
    tst::write_bytes(td / "empty.gguf", {});
    PrefetchOptions o;
    o.buffers = 0;
    EXPECT_EQ(prefetch_and_verify(td / "m.gguf", o).error().code, ErrorCode::InvalidArgument);
    o = {};
    o.chunk_bytes = 4096;
    EXPECT_EQ(prefetch_and_verify(td / "m.gguf", o).error().code, ErrorCode::InvalidArgument);
    o = {};
    o.expected_sha256 = "zz";
    EXPECT_EQ(prefetch_and_verify(td / "m.gguf", o).error().code, ErrorCode::InvalidArgument);
    o = {};
    EXPECT_FALSE(prefetch_and_verify(td / "absent.gguf", o).ok());
    EXPECT_EQ(prefetch_and_verify(td / "empty.gguf", o).error().code, ErrorCode::ModelError);
}

TEST(VaultPrefetch, CancelledTokenStopsTheRead) {
    hcs::test::TempDir td;
    tst::write_bytes(td / "m.gguf", tst::random_bytes(8 * kMiB, 5));
    hcs::CancellationSource cs;
    cs.request_cancel();
    PrefetchOptions o;
    o.chunk_bytes = kMiB;
    o.cancel = cs.token();
    auto r = prefetch_and_verify(td / "m.gguf", o);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
}
