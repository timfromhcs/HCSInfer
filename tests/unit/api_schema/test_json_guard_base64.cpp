// Direct tests of the header-only helpers hcs/api/json_guard.hpp and hcs/api/base64.hpp.
#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <vector>

#include "hcs/api/base64.hpp"
#include "hcs/api/json_guard.hpp"

using namespace hcs::api;

namespace {
std::string b64(const std::string& s) {
    return base64_encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}
}  // namespace

TEST(Base64, Rfc4648Vectors) {
    EXPECT_EQ(b64(""), "");
    EXPECT_EQ(b64("f"), "Zg==");
    EXPECT_EQ(b64("fo"), "Zm8=");
    EXPECT_EQ(b64("foo"), "Zm9v");
    EXPECT_EQ(b64("foob"), "Zm9vYg==");
    EXPECT_EQ(b64("fooba"), "Zm9vYmE=");
    EXPECT_EQ(b64("foobar"), "Zm9vYmFy");
}

TEST(Base64, BinaryAlphabetEdgesAndLength) {
    const std::uint8_t a[] = {0xfb, 0xff, 0xbf};  // exercises '+' and '/'
    EXPECT_EQ(base64_encode(a, 3), "+/+/");
    const std::uint8_t z[] = {0x00, 0x00, 0x00};
    EXPECT_EQ(base64_encode(z, 3), "AAAA");
    for (std::size_t n = 0; n < 40; ++n) {
        std::vector<std::uint8_t> v(n, 0xA5);
        const auto e = base64_encode(v.data(), v.size());
        EXPECT_EQ(e.size(), (n + 2) / 3 * 4) << n;
        for (char c : e)
            EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=');
    }
}

TEST(Base64, ZeroLengthWithNullPointerIsSafe) {
    EXPECT_EQ(base64_encode(nullptr, 0), "");
}

TEST(JsonGuard, AcceptsShallowAndRejectsDeep) {
    EXPECT_TRUE(json_depth_ok("{}", 1));
    EXPECT_TRUE(json_depth_ok("[[[1]]]", 3));
    EXPECT_FALSE(json_depth_ok("[[[1]]]", 2));
    EXPECT_TRUE(json_depth_ok("42", 0));
    EXPECT_TRUE(json_depth_ok("", 0));
}

TEST(JsonGuard, BracketsInsideStringsDoNotCount) {
    EXPECT_TRUE(json_depth_ok("{\"a\":\"[[[[[[[[[[\",\"b\":\"{{{{{{\"}", 1));
    // An escaped quote and an escaped backslash inside strings.
    EXPECT_TRUE(json_depth_ok("[\"\\\"[[[[\", \"\\\\\", \"[[[\"]", 1));
    // A backslash-escaped backslash right before the closing quote ends the string; [[1]] then counts.
    EXPECT_FALSE(json_depth_ok("[\"\\\\\",[[1]]]", 2));
}

TEST(JsonGuard, UnbalancedInputIsDecidedWithoutCrashing) {
    EXPECT_FALSE(json_depth_ok(std::string(10000, '['), kMaxJsonDepth));
    EXPECT_TRUE(json_depth_ok(std::string(10000, ']'), kMaxJsonDepth));  // closing only: depth goes negative
    EXPECT_FALSE(json_depth_ok("{\"a\":[", 1));
}

TEST(JsonGuard, BoundaryAtMaxDepth) {
    const std::string ok = std::string(kMaxJsonDepth, '[') + std::string(kMaxJsonDepth, ']');
    const std::string deep = std::string(kMaxJsonDepth + 1, '[') + std::string(kMaxJsonDepth + 1, ']');
    EXPECT_TRUE(json_depth_ok(ok, kMaxJsonDepth));
    EXPECT_FALSE(json_depth_ok(deep, kMaxJsonDepth));
}

TEST(JsonGuard, VeryDeepDocumentIsFlaggedBeforeAnyRecursiveParse) {
    const std::string deep = std::string(20000, '[') + std::string(20000, ']');
    EXPECT_FALSE(json_depth_ok(deep, kMaxJsonDepth));
}
