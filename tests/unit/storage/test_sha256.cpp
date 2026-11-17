#include <gtest/gtest.h>

#include <string>

#include "hcs/storage/sha256.hpp"
#include "storage_test_util.hpp"

using namespace hcs::storage;

namespace {
std::string hex_of(const std::string& s) {
    return to_hex(sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size())));
}
struct ForcePortable {
    explicit ForcePortable(bool f) { sha256_force_portable(f); }
    ~ForcePortable() { sha256_force_portable(false); }
};
}  // namespace

TEST(Sha256, KnownVectorsDefaultImplementation) {
    EXPECT_EQ(hex_of(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex_of("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, KnownVectorsPortableImplementation) {
    ForcePortable fp(true);
    EXPECT_STREQ(sha256_implementation(), "portable");
    EXPECT_EQ(hex_of(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex_of("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, MillionAs) {
    const std::string m(1000000, 'a');
    EXPECT_EQ(hex_of(m), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    ForcePortable fp(true);
    EXPECT_EQ(hex_of(m), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, HardwareMatchesPortableOnManyLengthsAndSplits) {
    const auto data = test::random_bytes(5000, 7);
    for (std::size_t len : {0u, 1u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 127u, 128u, 129u, 1000u, 4999u, 5000u}) {
        const std::span<const std::uint8_t> s(data.data(), len);
        Digest hw = sha256(s);
        Digest sw;
        {
            ForcePortable fp(true);
            sw = sha256(s);
        }
        EXPECT_EQ(hw, sw) << "len=" << len;
        // Incremental update in odd pieces must give the same digest.
        Sha256 h;
        std::size_t pos = 0;
        std::size_t step = 1;
        while (pos < len) {
            const std::size_t n = std::min(step, len - pos);
            h.update(s.data() + pos, n);
            pos += n;
            step = step * 3 + 1;
        }
        EXPECT_EQ(h.finalize(), sw) << "incremental len=" << len;
    }
}

TEST(Sha256, HexRoundTrip) {
    const Digest d = sha256(test::random_bytes(10, 1));
    Digest back{};
    ASSERT_TRUE(digest_from_hex(to_hex(d), back));
    EXPECT_EQ(d, back);
    EXPECT_FALSE(digest_from_hex("zz", back));
    EXPECT_FALSE(digest_from_hex(std::string(64, 'g'), back));
}
