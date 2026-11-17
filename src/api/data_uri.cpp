#include "hcs/api/data_uri.hpp"

#include <array>
#include <cstdint>

namespace hcs::api {
namespace {

constexpr std::array<std::int8_t, 256> make_table() {
    std::array<std::int8_t, 256> t{};
    for (auto& v : t) v = -1;
    for (int i = 0; i < 26; ++i) {
        t[static_cast<std::size_t>('A' + i)] = static_cast<std::int8_t>(i);
        t[static_cast<std::size_t>('a' + i)] = static_cast<std::int8_t>(26 + i);
    }
    for (int i = 0; i < 10; ++i) t[static_cast<std::size_t>('0' + i)] = static_cast<std::int8_t>(52 + i);
    t[static_cast<std::size_t>('+')] = 62;
    t[static_cast<std::size_t>('/')] = 63;
    return t;
}

}  // namespace

Result<ImageInput> decode_image_data_uri(std::string_view uri, std::size_t max_bytes) {
    constexpr std::string_view kPrefix = "data:image/";
    if (uri.size() < kPrefix.size() || uri.substr(0, kPrefix.size()) != kPrefix)
        return make_error(ErrorCode::InvalidArgument,
                          "image_url must be a data:image/...;base64,... URI (remote URLs are not fetched)");
    const std::size_t semi = uri.find(';', kPrefix.size());
    const std::size_t comma = uri.find(',', kPrefix.size());
    if (semi == std::string_view::npos || comma == std::string_view::npos || semi > comma)
        return make_error(ErrorCode::InvalidArgument, "malformed data URI header");
    const std::string_view type = uri.substr(kPrefix.size(), semi - kPrefix.size());
    if (type != "png" && type != "jpeg" && type != "jpg" && type != "gif" && type != "bmp")
        return make_error(ErrorCode::InvalidArgument,
                          "unsupported image media type (png, jpeg, gif and bmp are supported)");
    if (uri.substr(semi, comma - semi) != ";base64")
        return make_error(ErrorCode::InvalidArgument, "data URI must use ;base64 encoding");
    const std::string_view b64 = uri.substr(comma + 1);
    if (b64.empty() || b64.size() % 4 != 0) return make_error(ErrorCode::InvalidArgument, "malformed base64 payload");
    std::size_t pad = 0;
    if (b64[b64.size() - 1] == '=') ++pad;
    if (b64.size() >= 2 && b64[b64.size() - 2] == '=') ++pad;
    const std::size_t decoded = b64.size() / 4 * 3 - pad;
    if (decoded > max_bytes)
        return make_error(ErrorCode::InvalidArgument,
                          "image exceeds the " + std::to_string(max_bytes) + " byte limit");

    static constexpr auto table = make_table();
    ImageInput out;
    out.bytes.reserve(decoded);
    for (std::size_t i = 0; i < b64.size(); i += 4) {
        std::uint32_t v = 0;
        int real = 4;
        for (int k = 0; k < 4; ++k) {
            const unsigned char c = static_cast<unsigned char>(b64[i + static_cast<std::size_t>(k)]);
            if (c == '=') {
                // padding only in the last quantum, last one/two positions
                if (i + 4 != b64.size() || k < 2) return make_error(ErrorCode::InvalidArgument, "malformed base64 padding");
                --real;
                v <<= 6;
                continue;
            }
            const std::int8_t d = table[c];
            if (d < 0) return make_error(ErrorCode::InvalidArgument, "malformed base64 payload (bad character)");
            if (real < 4) return make_error(ErrorCode::InvalidArgument, "malformed base64 padding");
            v = (v << 6) | static_cast<std::uint32_t>(d);
        }
        out.bytes.push_back(static_cast<std::uint8_t>(v >> 16));
        if (real > 2) out.bytes.push_back(static_cast<std::uint8_t>(v >> 8));
        if (real > 3) out.bytes.push_back(static_cast<std::uint8_t>(v));
    }
    return out;
}

}  // namespace hcs::api
