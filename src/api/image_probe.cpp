#include "hcs/api/image_probe.hpp"

#include <cstring>

namespace hcs::api {
namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
std::uint32_t be16(const std::uint8_t* p) { return (static_cast<std::uint32_t>(p[0]) << 8) | p[1]; }
std::uint32_t le16(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

std::optional<ImageProbe> probe_image(const std::uint8_t* d, std::size_t n) {
    if (!d) return std::nullopt;
    ImageProbe out;
    // PNG: signature, then IHDR must be the first chunk.
    static const std::uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 8 && std::memcmp(d, kPng, 8) == 0) {
        if (n < 24 || std::memcmp(d + 12, "IHDR", 4) != 0 || be32(d + 8) != 13) return std::nullopt;
        out.format = "png";
        out.width = be32(d + 16);
        out.height = be32(d + 20);
    } else if (n >= 10 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0)) {
        out.format = "gif";
        out.width = le16(d + 6);
        out.height = le16(d + 8);
    } else if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
        const std::int32_t w = static_cast<std::int32_t>(le32(d + 18));
        const std::int32_t h = static_cast<std::int32_t>(le32(d + 22));
        if (w <= 0 || h == INT32_MIN) return std::nullopt;
        out.format = "bmp";
        out.width = static_cast<std::uint32_t>(w);
        out.height = static_cast<std::uint32_t>(h < 0 ? -h : h);
    } else if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        // JPEG: walk marker segments until a start-of-frame; bounded by the buffer.
        std::size_t i = 2;
        bool found = false;
        while (i + 4 <= n) {
            if (d[i] != 0xFF) { ++i; continue; }
            const std::uint8_t m = d[i + 1];
            if (m == 0xFF) { ++i; continue; }  // fill byte
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }  // no payload
            if (m == 0xD9 || m == 0xDA) break;  // EOI / SOS before any SOF: not a valid frame header
            const std::uint32_t len = be16(d + i + 2);
            if (len < 2) return std::nullopt;
            const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
            if (sof) {
                if (i + 9 > n) return std::nullopt;
                out.format = "jpeg";
                out.height = be16(d + i + 5);
                out.width = be16(d + i + 7);
                found = true;
                break;
            }
            i += 2 + len;
        }
        if (!found) return std::nullopt;
    } else {
        return std::nullopt;
    }
    if (out.width == 0 || out.height == 0) return std::nullopt;
    return out;
}

}  // namespace hcs::api
