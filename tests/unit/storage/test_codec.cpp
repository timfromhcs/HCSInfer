#include <gtest/gtest.h>

#include "hcs/storage/codec.hpp"
#include "storage_test_util.hpp"

using namespace hcs::storage;

namespace {
class CodecRoundTrip : public ::testing::TestWithParam<std::pair<Codec, int>> {};
}  // namespace

TEST_P(CodecRoundTrip, BitExactForCompressibleRandomAndTinyInputs) {
    const auto [codec, level] = GetParam();
    for (std::size_t n : {1u, 2u, 17u, 4096u, 100000u, 1u << 20}) {
        for (int kind = 0; kind < 2; ++kind) {
            const auto src = kind ? test::random_bytes(n, n) : test::compressible_bytes(n, n);
            std::vector<std::uint8_t> comp;
            ASSERT_TRUE(compress(codec, level, src, comp).ok());
            EXPECT_LE(comp.size(), compress_bound(codec, n));
            std::vector<std::uint8_t> out(n);
            auto r = decompress(codec, comp, out);
            ASSERT_TRUE(r.ok()) << r.error().message;
            EXPECT_EQ(out, src) << codec_name(codec) << " n=" << n << " kind=" << kind;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(All, CodecRoundTrip,
                         ::testing::Values(std::pair{Codec::Raw, 0}, std::pair{Codec::Lz4, 0},
                                           std::pair{Codec::Lz4, 9}, std::pair{Codec::Zstd, 0},
                                           std::pair{Codec::Zstd, 1}, std::pair{Codec::Zstd, 12}));

TEST(Codec, CompressibleDataActuallyShrinks) {
    const auto src = test::compressible_bytes(1 << 20, 3);
    for (Codec c : {Codec::Lz4, Codec::Zstd}) {
        std::vector<std::uint8_t> comp;
        ASSERT_TRUE(compress(c, 0, src, comp).ok());
        EXPECT_LT(comp.size(), src.size() / 2) << codec_name(c);
    }
}

TEST(Codec, CorruptBlockIsRejectedNotCrashing) {
    const auto src = test::compressible_bytes(200000, 5);
    for (Codec c : {Codec::Lz4, Codec::Zstd}) {
        std::vector<std::uint8_t> comp;
        ASSERT_TRUE(compress(c, 0, src, comp).ok());
        int rejected = 0, silently_wrong = 0;
        for (std::size_t pos = 0; pos < comp.size(); pos += comp.size() / 64 + 1) {
            auto bad = comp;
            bad[pos] ^= 0xFF;
            std::vector<std::uint8_t> out(src.size());
            auto r = decompress(c, bad, out);
            if (!r.ok())
                ++rejected;
            else if (out != src)
                ++silently_wrong;  // codecs have no checksum here; the chunk SHA-256 catches this
        }
        EXPECT_GT(rejected + silently_wrong, 0) << codec_name(c);
        // truncated stream
        std::vector<std::uint8_t> out(src.size());
        EXPECT_FALSE(decompress(c, std::span<const std::uint8_t>(comp.data(), comp.size() / 2), out).ok());
        // wrong declared raw size (too small and too large)
        std::vector<std::uint8_t> small(src.size() - 1), big(src.size() + 1);
        EXPECT_FALSE(decompress(c, comp, small).ok());
        EXPECT_FALSE(decompress(c, comp, big).ok());
        // garbage input
        const auto junk = test::random_bytes(1000, 99);
        EXPECT_FALSE(decompress(c, junk, out).ok());
    }
}

TEST(Codec, NamesAndIds) {
    EXPECT_EQ(parse_codec("zstd"), Codec::Zstd);
    EXPECT_EQ(parse_codec("lz4"), Codec::Lz4);
    EXPECT_FALSE(parse_codec("gzip").has_value());
    EXPECT_FALSE(codec_from_id(3).has_value());
    EXPECT_FALSE(lz4_version().empty());
    EXPECT_FALSE(zstd_version().empty());
}
