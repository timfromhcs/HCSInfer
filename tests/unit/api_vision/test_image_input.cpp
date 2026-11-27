// Unit tests for the untrusted-image front door: header sniffing (no decode) and data-URI decoding.
#include <gtest/gtest.h>

#include <string>

#include "hcs/api/data_uri.hpp"
#include "hcs/api/image_probe.hpp"
#include "test_png.hpp"

using namespace hcs;
using namespace hcs::api;

TEST(ImageProbe, PngDimensionsFromHeaderOnly) {
    const auto png = test::make_png_header_only(7000, 3000);  // no usable pixel data
    auto p = probe_image(png.data(), png.size());
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->format, "png");
    EXPECT_EQ(p->width, 7000u);
    EXPECT_EQ(p->height, 3000u);
    EXPECT_EQ(p->pixels(), 21000000ull);
}

TEST(ImageProbe, RealGeneratedPng) {
    const auto png = test::make_red_square_png(96);
    auto p = probe_image(png.data(), png.size());
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->width, 96u);
    EXPECT_EQ(p->height, 96u);
}

TEST(ImageProbe, GifBmpJpeg) {
    const std::uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a', 0x10, 0x00, 0x20, 0x00, 0, 0};
    auto g = probe_image(gif, sizeof gif);
    ASSERT_TRUE(g);
    EXPECT_EQ(g->format, "gif");
    EXPECT_EQ(g->width, 16u);
    EXPECT_EQ(g->height, 32u);

    std::uint8_t bmp[30] = {'B', 'M'};
    bmp[18] = 40;                                                    // width 40
    bmp[22] = 0xD8, bmp[23] = 0xFF, bmp[24] = 0xFF, bmp[25] = 0xFF;  // height -40 (top-down)
    auto b = probe_image(bmp, sizeof bmp);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->format, "bmp");
    EXPECT_EQ(b->width, 40u);
    EXPECT_EQ(b->height, 40u);

    // SOI, APP0 (len 4), SOF0: height 48, width 64
    const std::uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xC0, 0x00,
                                0x0B, 0x08, 0x00, 0x30, 0x00, 0x40, 0x01, 0x01, 0x11, 0x00};
    auto j = probe_image(jpg, sizeof jpg);
    ASSERT_TRUE(j);
    EXPECT_EQ(j->format, "jpeg");
    EXPECT_EQ(j->width, 64u);
    EXPECT_EQ(j->height, 48u);
}

TEST(ImageProbe, RejectsGarbageTruncatedAndZeroSized) {
    EXPECT_FALSE(probe_image(nullptr, 10));
    const std::uint8_t junk[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    EXPECT_FALSE(probe_image(junk, sizeof junk));
    auto png = test::make_png_header_only(10, 10);
    EXPECT_FALSE(probe_image(png.data(), 20));  // truncated IHDR
    auto zero = test::make_png_header_only(0, 10);
    EXPECT_FALSE(probe_image(zero.data(), zero.size()));
    const std::uint8_t jpg_no_sof[] = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02};
    EXPECT_FALSE(probe_image(jpg_no_sof, sizeof jpg_no_sof));
    const std::uint8_t jpg_bad_len[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x00, 0, 0};
    EXPECT_FALSE(probe_image(jpg_bad_len, sizeof jpg_bad_len));
}

TEST(DataUri, RoundTripsBytes) {
    const auto png = test::make_red_square_png(32);
    for (std::size_t cut : {0u, 1u, 2u}) {  // all three padding cases
        std::vector<std::uint8_t> v(png.begin(), png.end() - static_cast<std::ptrdiff_t>(cut));
        auto r = decode_image_data_uri(test::png_data_uri(v), 1u << 20);
        ASSERT_TRUE(r.ok()) << r.error().message;
        EXPECT_EQ(r.value().bytes, v);
    }
}

TEST(DataUri, RejectsEverythingThatIsNotAStrictImageDataUri) {
    const std::size_t cap = 1u << 20;
    auto bad = [&](const std::string& u) {
        auto r = decode_image_data_uri(u, cap);
        EXPECT_FALSE(r.ok()) << u;
        if (!r.ok()) {
            EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << u;
        }
    };
    bad("");
    bad("http://example.com/a.png");
    bad("https://example.com/a.png");
    bad("file:///C:/Windows/win.ini");
    bad(R"(C:\Windows\win.ini)");
    bad("data:text/plain;base64,QUJD");
    bad("data:image/svg+xml;base64,QUJD");
    bad("data:image/png,QUJD");             // not base64
    bad("data:image/png;base64,");          // empty
    bad("data:image/png;base64,QUJ");       // length % 4
    bad("data:image/png;base64,QU Jx");     // whitespace
    bad("data:image/png;base64,QUJ*");      // bad char
    bad("data:image/png;base64,QU==QUJD");  // padding in the middle
    bad("data:image/png;base64,Q===");      // too much padding
    bad("data:image/png;charset=x;base64,QUJD");
}

TEST(DataUri, SizeLimitIsCheckedBeforeDecoding) {
    const std::string uri = "data:image/png;base64," + std::string(4000, 'A');  // decodes to 3000 bytes
    EXPECT_TRUE(decode_image_data_uri(uri, 3000).ok());
    auto r = decode_image_data_uri(uri, 2999);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}
