#include <gtest/gtest.h>

#include <limits>

#include "hcs/storage/gguf.hpp"
#include "storage_test_util.hpp"

using namespace hcs;
using namespace hcs::storage;
namespace tst = hcs::storage::test;

namespace {
Result<GgufInfo> parse(const std::vector<std::uint8_t>& b) {
    MemorySource s(b);
    return parse_gguf(s);
}
void put64(std::vector<std::uint8_t>& b, std::size_t pos, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[pos + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
}  // namespace

TEST(Gguf, ParsesValidFile) {
    auto r = parse(tst::make_gguf());
    ASSERT_TRUE(r.ok()) << r.error().message;
    const auto& g = r.value();
    EXPECT_EQ(g.version, 3u);
    EXPECT_EQ(g.tensor_count, 2u);
    EXPECT_EQ(g.architecture, "testarch");
    EXPECT_EQ(g.name, "tiny");
    ASSERT_EQ(g.tensors.size(), 2u);
    EXPECT_EQ(g.tensors[0].name, "t0");
    EXPECT_EQ(g.tensors[0].byte_size, 32u);
    EXPECT_EQ(g.tensors[1].byte_size, 136u);
    EXPECT_TRUE(g.tensors[1].size_known);
    EXPECT_EQ(g.data_offset % 32, 0u);
    EXPECT_EQ(parse(tst::make_gguf(2)).value().version, 2u);
}

TEST(Gguf, RejectsBadMagicAndVersions) {
    auto b = tst::make_gguf();
    b[0] = 'X';
    EXPECT_EQ(parse(b).error().code, ErrorCode::ModelError);
    EXPECT_EQ(parse(tst::make_gguf(1)).error().code, ErrorCode::Unsupported);
    EXPECT_EQ(parse(tst::make_gguf(99)).error().code, ErrorCode::Unsupported);
}

TEST(Gguf, TruncationAtEveryPositionIsRejectedSafely) {
    const auto good = tst::make_gguf();
    for (std::size_t n = 0; n + 1 < good.size(); n += 3) {
        std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        auto r = parse(cut);
        EXPECT_FALSE(r.ok()) << "len=" << n;  // tensors need their data, so any cut is an error
    }
}

TEST(Gguf, HugeCountsAreRejectedBeforeAllocation) {
    auto b = tst::make_gguf();
    put64(b, 8, std::numeric_limits<std::uint64_t>::max());  // tensor count
    EXPECT_EQ(parse(b).error().code, ErrorCode::ModelError);
    b = tst::make_gguf();
    put64(b, 8, 1u << 19);  // within the cap but far beyond what the file can hold
    EXPECT_EQ(parse(b).error().code, ErrorCode::ModelError);
    b = tst::make_gguf();
    put64(b, 16, 1ull << 40);  // kv count
    EXPECT_EQ(parse(b).error().code, ErrorCode::ModelError);
}

TEST(Gguf, HugeStringLengthIsRejected) {
    auto b = tst::make_gguf();
    // First key length field sits at offset 24.
    put64(b, 24, 1ull << 62);
    EXPECT_EQ(parse(b).error().code, ErrorCode::ModelError);
    b = tst::make_gguf();
    put64(b, 24, 70000);  // above the key cap
    EXPECT_EQ(parse(b).error().code, ErrorCode::ModelError);
}

TEST(Gguf, TensorBeyondFileOrMisalignedOrBadDims) {
    tst::GgufBuilder g;
    auto build = [&](std::uint32_t nd, std::uint64_t d0, std::uint32_t type, std::uint64_t off, std::size_t data_bytes) {
        g.b.clear();
        g.raw("GGUF", 4); g.u32(3); g.u64(1); g.u64(0);
        g.str("w"); g.u32(nd);
        for (std::uint32_t i = 0; i < nd; ++i) g.u64(i == 0 ? d0 : 2);
        g.u32(type); g.u64(off);
        while (g.b.size() % 32) g.b.push_back(0);
        g.b.resize(g.b.size() + data_bytes, 0);
        return parse(g.b);
    };
    EXPECT_TRUE(build(1, 8, 0, 0, 32).ok());
    EXPECT_FALSE(build(1, 8, 0, 0, 31).ok()) << "data section shorter than the tensor";
    EXPECT_FALSE(build(1, 8, 0, 64, 32).ok()) << "offset beyond data";
    EXPECT_FALSE(build(1, 8, 0, 3, 64).ok()) << "misaligned offset";
    EXPECT_FALSE(build(0, 8, 0, 0, 32).ok()) << "zero dims";
    EXPECT_FALSE(build(5, 8, 0, 0, 1024).ok()) << ">4 dims";
    EXPECT_FALSE(build(1, 0, 0, 0, 32).ok()) << "zero-sized dim";
    EXPECT_FALSE(build(1, 33, 8, 0, 1024).ok()) << "Q8_0 row not a multiple of 32";
    EXPECT_FALSE(build(2, std::numeric_limits<std::uint64_t>::max() / 2, 0, 0, 64).ok()) << "size overflow";
    EXPECT_TRUE(build(1, 8, 9999, 0, 32).ok()) << "unknown tensor types are tolerated (size unchecked)";
}

TEST(Gguf, ArrayDepthAndLengthLimits) {
    tst::GgufBuilder g;
    g.raw("GGUF", 4); g.u32(3); g.u64(0); g.u64(1);
    g.str("deep");
    g.u32(9);
    for (int i = 0; i < 8; ++i) { g.u32(9); g.u64(1); }  // nested arrays beyond the depth cap
    g.u32(4); g.u64(1); g.u32(0);
    EXPECT_FALSE(parse(g.b).ok());

    tst::GgufBuilder h;
    h.raw("GGUF", 4); h.u32(3); h.u64(0); h.u64(1);
    h.str("arr"); h.u32(9); h.u32(4); h.u64(1ull << 40);  // 2^40 u32 elements in a tiny file
    h.b.resize(h.b.size() + 64, 0);
    EXPECT_FALSE(parse(h.b).ok());

    tst::GgufBuilder s;
    s.raw("GGUF", 4); s.u32(3); s.u64(0); s.u64(1);
    s.str("arr"); s.u32(9); s.u32(8); s.u64(1ull << 30);  // 2^30 strings
    s.b.resize(s.b.size() + 64, 0);
    EXPECT_FALSE(parse(s.b).ok());

    tst::GgufBuilder u;
    u.raw("GGUF", 4); u.u32(3); u.u64(0); u.u64(1);
    u.str("k"); u.u32(77);  // unknown value type
    u.b.resize(u.b.size() + 64, 0);
    EXPECT_FALSE(parse(u.b).ok());
}

TEST(Gguf, InvalidAlignment) {
    tst::GgufBuilder g;
    g.raw("GGUF", 4); g.u32(3); g.u64(0); g.u64(1);
    g.str("general.alignment"); g.u32(4); g.u32(33);
    g.b.resize(g.b.size() + 64, 0);
    EXPECT_FALSE(parse(g.b).ok());
    tst::GgufBuilder z;
    z.raw("GGUF", 4); z.u32(3); z.u64(0); z.u64(1);
    z.str("general.alignment"); z.u32(4); z.u32(0);
    z.b.resize(z.b.size() + 64, 0);
    EXPECT_FALSE(parse(z.b).ok());
}

TEST(Gguf, TypeLayoutTable) {
    std::uint64_t be, bb;
    ASSERT_TRUE(ggml_type_layout(12, be, bb));  // Q4_K
    EXPECT_EQ(be, 256u);
    EXPECT_EQ(bb, 144u);
    EXPECT_FALSE(ggml_type_layout(9999, be, bb));
}
