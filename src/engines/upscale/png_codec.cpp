// PNG decode/encode glue for the upscaler (stb, public domain / MIT, from the pinned stable-diffusion.cpp
// thirdparty tree). The stb implementations are made `static` so this translation unit never clashes with the
// diffusion adapter, which defines the same headers' implementations with external linkage.
#include <cstring>

#include "hcs/upscale.hpp"

#ifdef HCS_UPSCALE_HAVE_STB
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#endif

namespace hcs::upscale {

#ifdef HCS_UPSCALE_HAVE_STB

namespace {
void png_sink(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
    const auto* p = static_cast<const std::uint8_t*>(data);
    v->insert(v->end(), p, p + size);
}
}  // namespace

Result<Rgb8> decode_png(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 8 || size > (256u << 20))
        return make_error(ErrorCode::InvalidArgument, "not a PNG (empty or oversized input)");
    int w = 0, h = 0, c = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &c) || w <= 0 || h <= 0)
        return make_error(ErrorCode::InvalidArgument, "the image is not a decodable PNG");
    if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > kMaxSourcePixels)
        return make_error(ErrorCode::InvalidArgument, "PNG exceeds the upscaler input pixel limit");
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &c, 3);
    if (!px)
        return make_error(ErrorCode::InvalidArgument, "PNG decoding failed");
    Rgb8 out;
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.pixels.assign(px, px + static_cast<std::size_t>(w) * h * 3);
    stbi_image_free(px);
    return out;
}

Result<std::vector<std::uint8_t>> encode_png(const Rgb8& img) {
    if (img.width == 0 || img.height == 0 ||
        img.pixels.size() != static_cast<std::size_t>(img.width) * img.height * 3)
        return make_error(ErrorCode::InvalidArgument, "cannot encode an empty or inconsistent image");
    std::vector<std::uint8_t> out;
    if (!stbi_write_png_to_func(png_sink, &out, static_cast<int>(img.width), static_cast<int>(img.height), 3,
                                img.pixels.data(), static_cast<int>(img.width) * 3))
        return make_error(ErrorCode::Internal, "PNG encoding failed");
    return out;
}

#else

Result<Rgb8> decode_png(const std::uint8_t*, std::size_t) {
    return make_error(ErrorCode::Unsupported, "this build has no PNG codec (stb headers were not found)");
}
Result<std::vector<std::uint8_t>> encode_png(const Rgb8&) {
    return make_error(ErrorCode::Unsupported, "this build has no PNG codec (stb headers were not found)");
}

#endif

Result<std::vector<std::uint8_t>> upscale_png(IUpscaleEngine& engine, const std::vector<std::uint8_t>& png,
                                              const Options& opt, const CancellationToken& token,
                                              const ProgressFn& progress, Stats* stats) {
    auto src = decode_png(png.data(), png.size());
    if (!src)
        return src.error();
    auto up = upscale_image(engine, src.value(), opt, token, progress, stats);
    if (!up)
        return up.error();
    if (stats && stats->skipped)
        return png;
    return encode_png(up.value());
}

}  // namespace hcs::upscale
