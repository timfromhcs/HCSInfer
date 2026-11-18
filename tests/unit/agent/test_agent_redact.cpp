#include <gtest/gtest.h>

#include <string>

#include "hcs/agent/agent_redact.hpp"

using namespace hcs::agent;

TEST(AgentRedact, RemovesSessionTokenLiteralEverywhere) {
    const std::string token = "hcs-session-token-0123456789";
    const std::string out = redact_secrets("a " + token + " b " + token + " c", {token});
    EXPECT_EQ(out.find(token), std::string::npos);
    EXPECT_EQ(out, "a [redacted] b [redacted] c");
}

TEST(AgentRedact, IgnoresTooShortLiteralsSoOrdinaryTextSurvives) {
    EXPECT_EQ(redact_secrets("the cat sat", {"cat", ""}), "the cat sat");
}

TEST(AgentRedact, RemovesCommonCredentialShapes) {
    for (const char* secret :
         {"sk-abcdefghijklmnop1234", "ghp_abcdefghijklmnop1234", "hf_abcdefghijklmnopqrst",
          "github_pat_11ABCDEFG0123456789", "AKIAABCDEFGHIJKLMNOP", "xoxb-1234567890-abcdefghij"}) {
        const std::string out = redact_secrets(std::string("key=") + secret + " end");
        EXPECT_EQ(out.find(secret), std::string::npos) << secret;
        EXPECT_NE(out.find("[redacted]"), std::string::npos) << secret;
        EXPECT_NE(out.find(" end"), std::string::npos) << secret;
    }
}

TEST(AgentRedact, BearerIsCaseInsensitiveAndKeepsShortValues) {
    EXPECT_EQ(redact_secrets("Authorization: BEARER abcdefghijklmnopqrstu"), "Authorization: [redacted]");
    EXPECT_EQ(redact_secrets("bearer abc"), "bearer abc");
}

TEST(AgentRedact, ShortPrefixLookalikesAreLeftAlone) {
    EXPECT_EQ(redact_secrets("sk-short and hf_x and ask-me"), "sk-short and hf_x and ask-me");
}

TEST(AgentRedact, HandlesEmptyAndSecretAtEndOfInput) {
    EXPECT_EQ(redact_secrets(""), "");
    EXPECT_EQ(redact_secrets("sk-abcdefghijklmnop1234"), "[redacted]");
}

TEST(AgentRedact, DoesNotLoopOnRedactionMarkerInInput) {
    const std::string in = "[redacted][redacted] sk-abcdefghijklmnop1234";
    EXPECT_EQ(redact_secrets(in, {"[redacted]x"}), "[redacted][redacted] [redacted]");
}

TEST(AgentRedact, CapUtf8NoTruncationWhenFits) {
    bool t = true;
    EXPECT_EQ(cap_utf8("hello", 5, t), "hello");
    EXPECT_FALSE(t);
    EXPECT_EQ(cap_utf8("", 0, t), "");
    EXPECT_FALSE(t);
}

TEST(AgentRedact, CapUtf8CutsOnCodePointBoundary) {
    // "a" + U+00E9 (2 bytes) + U+20AC (3 bytes) + U+1F600 (4 bytes)
    const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    ASSERT_EQ(s.size(), 10u);
    struct Case {
        std::size_t cap;
        std::string expect;
    };
    const Case cases[] = {{0, ""},
                          {1, "a"},
                          {2, "a"},  // would split U+00E9
                          {3, "a\xC3\xA9"},
                          {4, "a\xC3\xA9"},  // splits U+20AC
                          {5, "a\xC3\xA9"},
                          {6, "a\xC3\xA9\xE2\x82\xAC"},
                          {7, "a\xC3\xA9\xE2\x82\xAC"},  // splits emoji
                          {9, "a\xC3\xA9\xE2\x82\xAC"}};
    for (const auto& c : cases) {
        bool t = false;
        const std::string out = cap_utf8(s, c.cap, t);
        EXPECT_TRUE(t) << c.cap;
        EXPECT_EQ(out, c.expect) << c.cap;
        EXPECT_LE(out.size(), c.cap);
    }
}

TEST(AgentRedact, CapUtf8SurvivesInvalidUtf8Continuations) {
    const std::string s(64, static_cast<char>(0x80));  // only continuation bytes
    bool t = false;
    const std::string out = cap_utf8(s, 10, t);
    EXPECT_TRUE(t);
    EXPECT_TRUE(out.empty());
}
