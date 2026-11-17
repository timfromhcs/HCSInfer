// Real-model storage test. Needs HCS_TEST_GGUF=<path to a real .gguf>; otherwise it SKIPS and is
// reported as BLOCKED/NOT RUN (never as a pass). Read-only: nothing is written.
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "hcs/storage/model_reader.hpp"

using namespace hcs::storage;

namespace {
std::string model_path() {
    const char* p = std::getenv("HCS_TEST_GGUF");
    return p ? p : "";
}
}  // namespace

TEST(RealGguf, HeaderParsesAndTensorsLieInsideTheFile) {
    const auto path = model_path();
    if (path.empty())
        GTEST_SKIP() << "BLOCKED: HCS_TEST_GGUF is not set";
    auto r = PlainFileReader::open(path);
    ASSERT_TRUE(r.ok()) << r.error().message;
    ASSERT_TRUE(r.value()->gguf().has_value());
    const auto& g = *r.value()->gguf();
    EXPECT_GE(g.version, 2u);
    EXPECT_GT(g.tensor_count, 0u);
    EXPECT_EQ(g.tensors.size(), g.tensor_count);
    EXPECT_FALSE(g.architecture.empty());
    for (const auto& t : g.tensors) {
        if (t.size_known)
            EXPECT_LE(g.data_offset + t.offset + t.byte_size, g.file_size) << t.name;
    }
}

TEST(RealGguf, StreamingDeliversEveryByteInOrder) {
    const auto path = model_path();
    if (path.empty())
        GTEST_SKIP() << "BLOCKED: HCS_TEST_GGUF is not set";
    auto r = PlainFileReader::open(path, 4u << 20);
    ASSERT_TRUE(r.ok()) << r.error().message;
    std::uint64_t total = 0;
    StreamOptions so;
    so.buffers = 4;
    auto st = r.value()->stream(
        [&](std::size_t, const ChunkEntry& e, std::span<const std::uint8_t> d) -> hcs::Result<void> {
            EXPECT_EQ(e.raw_offset, total);
            total += d.size();
            return {};
        },
        so);
    ASSERT_TRUE(st.ok()) << st.error().message;
    EXPECT_EQ(total, r.value()->logical_size());
    EXPECT_LE(st.value().buffer_bytes_allocated, 4u * (4u << 20));
}
