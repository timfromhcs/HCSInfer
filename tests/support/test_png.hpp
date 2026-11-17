#pragma once
// Generated, redistributable test images (no third-party asset): valid PNGs built from stored (uncompressed)
// deflate blocks, so tests need no image library. Used by the vision unit/contract/integration tests.
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hcs::test {

inline std::uint32_t png_crc32(const std::uint8_t* d, std::size_t n, std::uint32_t crc = 0) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline void png_put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}

inline void png_chunk(std::vector<std::uint8_t>& out, const char* type,
                      const std::vector<std::uint8_t>& data) {
    png_put32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    png_put32(out, png_crc32(td.data(), td.size()));
}

// RGB 8-bit image, row callback-free: `pixel(x, y)` returns {r,g,b}.
template <class PixelFn>
std::vector<std::uint8_t> make_png_rgb(std::uint32_t w, std::uint32_t h, PixelFn&& pixel) {
    std::vector<std::uint8_t> raw;  // filter byte 0 + RGB per row
    raw.reserve(static_cast<std::size_t>(h) * (1 + 3 * w));
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto px = pixel(x, y);
            raw.push_back(px[0]);
            raw.push_back(px[1]);
            raw.push_back(px[2]);
        }
    }
    // zlib stream of stored blocks (<= 65535 bytes each)
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (auto c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t off = 0; off < raw.size() || off == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFF));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
                 raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
        if (last)
            break;
    }
    png_put32(z, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    png_put32(ihdr, w);
    png_put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, truecolour, deflate, no filter, no interlace
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", {});
    return png;
}

// A big solid red square on a white background (the real-model test asks for its colour).
inline std::vector<std::uint8_t> make_red_square_png(std::uint32_t size = 128) {
    return make_png_rgb(size, size, [size](std::uint32_t x, std::uint32_t y) {
        const bool in = x >= size / 4 && x < size * 3 / 4 && y >= size / 4 && y < size * 3 / 4;
        return std::array<std::uint8_t, 3>{in ? std::uint8_t{220} : std::uint8_t{255},
                                           in ? std::uint8_t{20} : std::uint8_t{255},
                                           in ? std::uint8_t{20} : std::uint8_t{255}};
    });
}

// Header-only PNG that declares huge dimensions but carries no pixels (pixel-limit tests).
inline std::vector<std::uint8_t> make_png_header_only(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    png_put32(ihdr, w);
    png_put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT",
              {0x00});  // stb_image's info scan needs a chunk after IHDR; still no usable pixel data
    return png;
}

inline std::string base64(const std::vector<std::uint8_t>& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (std::size_t i = 0; i < d.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(d[i]) << 16) |
                                (i + 1 < d.size() ? static_cast<std::uint32_t>(d[i + 1]) << 8 : 0u) |
                                (i + 2 < d.size() ? d[i + 2] : 0u);
        o.push_back(t[(v >> 18) & 63]);
        o.push_back(t[(v >> 12) & 63]);
        o.push_back(i + 1 < d.size() ? t[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < d.size() ? t[v & 63] : '=');
    }
    return o;
}

inline std::string png_data_uri(const std::vector<std::uint8_t>& png) {
    return "data:image/png;base64," + base64(png);
}

}  // namespace hcs::test
