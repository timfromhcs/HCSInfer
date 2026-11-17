#include <gtest/gtest.h>

#include <algorithm>
#include <regex>
#include <string>

#include "hcs/gui/gui_assets.hpp"

using namespace hcs::gui;

namespace {
std::string header(const GuiResponse& r, const std::string& name) {
    for (const auto& h : r.headers)
        if (h.first == name)
            return h.second;
    return {};
}
std::string asset_text(const std::string& name) {
    auto r = serve("GET", "/ui/" + name);
    EXPECT_TRUE(r.has_value());
    return r ? std::string(r->body) : std::string();
}
}  // namespace

TEST(GuiAssets, RootServesIndexHtmlWithStrictCsp) {
    auto r = serve("GET", "/");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 200);
    EXPECT_EQ(r->content_type, "text/html; charset=utf-8");
    EXPECT_NE(r->body.find("<title>HCSInfer</title>"), std::string_view::npos);
    const auto csp = header(*r, "Content-Security-Policy");
    EXPECT_EQ(csp, std::string(content_security_policy()));
    EXPECT_NE(csp.find("default-src 'none'"), std::string::npos);
    EXPECT_NE(csp.find("script-src 'self'"), std::string::npos);
    EXPECT_NE(csp.find("connect-src 'self'"), std::string::npos);
    EXPECT_NE(csp.find("frame-ancestors 'none'"), std::string::npos);
    EXPECT_NE(csp.find("base-uri 'none'"), std::string::npos);
    EXPECT_EQ(csp.find("unsafe-inline"), std::string::npos);
    EXPECT_EQ(csp.find("unsafe-eval"), std::string::npos);
    EXPECT_EQ(csp.find('*'), std::string::npos);
    EXPECT_EQ(csp.find("http"), std::string::npos);
    EXPECT_EQ(header(*r, "X-Frame-Options"), "DENY");
    EXPECT_EQ(header(*r, "Referrer-Policy"), "no-referrer");
    EXPECT_FALSE(header(*r, "Cross-Origin-Opener-Policy").empty());
}

TEST(GuiAssets, EveryAssetIsServedWithCspAndRightType) {
    const auto names = asset_names();
    ASSERT_GE(names.size(), 3u);
    for (auto n : names) {
        auto r = serve("GET", "/ui/" + std::string(n));
        ASSERT_TRUE(r.has_value()) << n;
        EXPECT_EQ(r->status, 200) << n;
        EXPECT_FALSE(r->body.empty()) << n;
        EXPECT_EQ(header(*r, "Content-Security-Policy"), std::string(content_security_policy())) << n;
    }
    EXPECT_EQ(serve("GET", "/ui/app.js")->content_type, "text/javascript; charset=utf-8");
    EXPECT_EQ(serve("GET", "/ui/app.css")->content_type, "text/css; charset=utf-8");
    EXPECT_EQ(serve("GET", "/ui/index.html")->content_type, "text/html; charset=utf-8");
}

TEST(GuiAssets, EmbeddedBytesMatchExpectedMarkers) {
    EXPECT_NE(asset_text("app.js").find("/hcs/v1/models"), std::string::npos);
    EXPECT_NE(asset_text("app.js").find("/v1/chat/completions"), std::string::npos);
    EXPECT_NE(asset_text("app.js").find("/v1/images/generations"), std::string::npos);
    EXPECT_NE(asset_text("app.js").find("/cancel"), std::string::npos);
    EXPECT_NE(asset_text("app.css").find("prefers-color-scheme"), std::string::npos);
}

TEST(GuiAssets, NoExternalUrlsOrInlineCode) {
    const std::regex ext_url(R"((https?:)?//[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
    const std::regex proto_rel(R"((src|href)\s*=\s*["']//)");
    const std::regex inline_handler(R"(\son[a-z]+\s*=)", std::regex::icase);
    for (auto n : asset_names()) {
        const std::string t = asset_text(std::string(n));
        EXPECT_FALSE(std::regex_search(t, ext_url)) << n;
        EXPECT_EQ(t.find("http://"), std::string::npos) << n;
        EXPECT_EQ(t.find("https://"), std::string::npos) << n;
        EXPECT_FALSE(std::regex_search(t, proto_rel)) << n;
        EXPECT_EQ(t.find("@import"), std::string::npos) << n;
        EXPECT_EQ(t.find("url(http"), std::string::npos) << n;
        EXPECT_EQ(t.find("fonts.googleapis"), std::string::npos) << n;
    }
    const std::string html = asset_text("index.html");
    EXPECT_FALSE(std::regex_search(html, inline_handler));
    EXPECT_EQ(html.find("<style"), std::string::npos);
    EXPECT_EQ(html.find(" style="), std::string::npos);
    // every <script> must reference a file
    std::size_t pos = 0;
    int scripts = 0;
    while ((pos = html.find("<script", pos)) != std::string::npos) {
        const auto end = html.find('>', pos);
        ASSERT_NE(end, std::string::npos);
        EXPECT_NE(html.substr(pos, end - pos).find("src=\"/ui/"), std::string::npos);
        ++scripts;
        pos = end;
    }
    EXPECT_EQ(scripts, 1);
    const std::string js = asset_text("app.js");
    for (const char* bad : {"eval(", "new Function", "document.write", "innerHTML", "outerHTML",
                            "insertAdjacentHTML", "XMLHttpRequest", "WebSocket", "EventSource"})
        EXPECT_EQ(js.find(bad), std::string::npos) << bad;
}

TEST(GuiAssets, TokenComesFromFragmentNeverQueryString) {
    const std::string js = asset_text("app.js");
    EXPECT_NE(js.find("location.hash"), std::string::npos);
    EXPECT_EQ(js.find("URLSearchParams"), std::string::npos);
    EXPECT_EQ(js.find("?token="), std::string::npos);
    // token is sent only as a header
    EXPECT_NE(js.find("\"Authorization\""), std::string::npos);
    EXPECT_EQ(js.find("console.log"), std::string::npos);
}

TEST(GuiAssets, TraversalAndMalformedPathsAreRejected) {
    for (const char* p : {"/ui/../secret", "/ui/..", "/ui/..%2f..%2fetc", "/ui/%2e%2e/x", "/ui/a/b",
                          "/ui/a\\b", "/ui/..\\x", "/ui/.hidden", "/ui//app.js", "/ui/app.js/",
                          "/ui/C:app.js", "/ui/app.js%00.png", "/ui/app js", "/ui/\x01"}) {
        auto r = serve("GET", p);
        ASSERT_TRUE(r.has_value()) << p;
        EXPECT_EQ(r->status, 400) << p;
        EXPECT_NE(r->content_type, "text/javascript; charset=utf-8") << p;
        EXPECT_FALSE(header(*r, "Content-Security-Policy").empty()) << p;
    }
    const std::string with_nul("/ui/app.js\0x", 12);
    EXPECT_EQ(serve("GET", with_nul)->status, 400);
    EXPECT_EQ(serve("GET", "/ui/" + std::string(200, 'a'))->status, 400);
}

TEST(GuiAssets, UnknownAssetIs404AndNamespaceEdges) {
    EXPECT_EQ(serve("GET", "/ui/nope.js")->status, 404);
    EXPECT_EQ(serve("GET", "/ui/")->status, 400);  // empty name
    EXPECT_EQ(serve("GET", "/ui")->status, 404);
    EXPECT_EQ(serve("GET", "/ui/index.htm")->status, 404);
}

TEST(GuiAssets, OtherPathsAreNotHandled) {
    for (const char* p : {"/v1/models", "/v1/chat/completions", "/hcs/v1/health", "/hcs/v1/jobs/job-1",
                          "/uix", "/ui-x/app.js", "/index.html", ""}) {
        EXPECT_FALSE(handles_path(p)) << p;
        EXPECT_FALSE(serve("GET", p).has_value()) << p;
    }
    EXPECT_TRUE(handles_path("/"));
    EXPECT_TRUE(handles_path("/ui/app.js"));
}

TEST(GuiAssets, MethodHandling) {
    auto head = serve("HEAD", "/");
    ASSERT_TRUE(head.has_value());
    EXPECT_EQ(head->status, 200);
    EXPECT_TRUE(head->body.empty());
    for (const char* m : {"POST", "PUT", "DELETE", "PATCH", "OPTIONS"}) {
        auto r = serve(m, "/");
        ASSERT_TRUE(r.has_value()) << m;
        EXPECT_EQ(r->status, 405) << m;
        EXPECT_EQ(header(*r, "Allow"), "GET, HEAD") << m;
        auto r2 = serve(m, "/ui/app.js");
        EXPECT_EQ(r2->status, 405) << m;
    }
}

TEST(GuiAssets, NoSecretsEmbedded) {
    for (auto n : asset_names()) {
        const std::string t = asset_text(std::string(n));
        EXPECT_EQ(t.find("HCS_API_TOKEN"), std::string::npos) << n;
    }
}

#include <fstream>
#include <iterator>

TEST(GuiAssets, EmbeddedBytesAreIdenticalToSourceFiles) {
    for (auto n : asset_names()) {
        std::ifstream f(std::string(HCS_GUI_ASSET_DIR) + "/" + std::string(n), std::ios::binary);
        ASSERT_TRUE(f.good()) << n;
        const std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(asset_text(std::string(n)), src) << n;
    }
}
