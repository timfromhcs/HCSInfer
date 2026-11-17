#include "hcs/storage/codec.hpp"

#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>

#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace hcs::storage {
namespace {

struct ZstdCctxDeleter {
    void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};
struct ZstdDctxDeleter {
    void operator()(ZSTD_DCtx* c) const { ZSTD_freeDCtx(c); }
};

ZSTD_CCtx* thread_cctx() {
    thread_local std::unique_ptr<ZSTD_CCtx, ZstdCctxDeleter> ctx(ZSTD_createCCtx());
    return ctx.get();
}
ZSTD_DCtx* thread_dctx() {
    thread_local std::unique_ptr<ZSTD_DCtx, ZstdDctxDeleter> ctx(ZSTD_createDCtx());
    return ctx.get();
}

Error model_err(std::string m) {
    return Error(ErrorCode::ModelError, std::move(m));
}

}  // namespace

std::string_view codec_name(Codec c) noexcept {
    switch (c) {
        case Codec::Raw:
            return "raw";
        case Codec::Lz4:
            return "lz4";
        case Codec::Zstd:
            return "zstd";
    }
    return "unknown";
}

std::optional<Codec> parse_codec(std::string_view n) noexcept {
    if (n == "raw")
        return Codec::Raw;
    if (n == "lz4")
        return Codec::Lz4;
    if (n == "zstd")
        return Codec::Zstd;
    return std::nullopt;
}

std::optional<Codec> codec_from_id(std::uint32_t id) noexcept {
    switch (id) {
        case 0:
            return Codec::Raw;
        case 1:
            return Codec::Lz4;
        case 2:
            return Codec::Zstd;
        default:
            return std::nullopt;
    }
}

std::size_t compress_bound(Codec c, std::size_t n) noexcept {
    switch (c) {
        case Codec::Raw:
            return n;
        case Codec::Lz4:
            if (n > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE))
                return 0;
            return static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(n)));
        case Codec::Zstd:
            return ZSTD_compressBound(n);
    }
    return 0;
}

Result<void> compress(Codec c, int level, std::span<const std::uint8_t> src, std::vector<std::uint8_t>& out) {
    const std::size_t bound = compress_bound(c, src.size());
    if (bound == 0 && !src.empty())
        return Error(ErrorCode::InvalidArgument, "input too large for codec");
    out.resize(bound);
    switch (c) {
        case Codec::Raw:
            if (!src.empty())
                std::memcpy(out.data(), src.data(), src.size());
            return {};
        case Codec::Lz4: {
            int n;
            if (level >= 3) {
                n = LZ4_compress_HC(reinterpret_cast<const char*>(src.data()),
                                    reinterpret_cast<char*>(out.data()), static_cast<int>(src.size()),
                                    static_cast<int>(out.size()),
                                    level > LZ4HC_CLEVEL_MAX ? LZ4HC_CLEVEL_MAX : level);
            } else {
                n = LZ4_compress_default(reinterpret_cast<const char*>(src.data()),
                                         reinterpret_cast<char*>(out.data()), static_cast<int>(src.size()),
                                         static_cast<int>(out.size()));
            }
            if (n <= 0 && !src.empty())
                return Error(ErrorCode::Internal, "LZ4 compression failed");
            out.resize(static_cast<std::size_t>(n));
            return {};
        }
        case Codec::Zstd: {
            const int lv = level == 0 ? 3 : level;
            const std::size_t n =
                ZSTD_compressCCtx(thread_cctx(), out.data(), out.size(), src.data(), src.size(), lv);
            if (ZSTD_isError(n))
                return Error(ErrorCode::Internal,
                             std::string("Zstd compression failed: ") + ZSTD_getErrorName(n));
            out.resize(n);
            return {};
        }
    }
    return Error(ErrorCode::Unsupported, "unknown codec");
}

Result<void> decompress(Codec c, std::span<const std::uint8_t> src, std::span<std::uint8_t> dst) {
    switch (c) {
        case Codec::Raw:
            if (src.size() != dst.size())
                return model_err("raw chunk size mismatch");
            if (!src.empty())
                std::memcpy(dst.data(), src.data(), src.size());
            return {};
        case Codec::Lz4: {
            if (src.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                dst.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return model_err("LZ4 chunk too large");
            const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(src.data()),
                                              reinterpret_cast<char*>(dst.data()),
                                              static_cast<int>(src.size()), static_cast<int>(dst.size()));
            if (n < 0)
                return model_err("corrupt LZ4 chunk");
            if (static_cast<std::size_t>(n) != dst.size())
                return model_err("LZ4 chunk decoded to unexpected size");
            return {};
        }
        case Codec::Zstd: {
            const std::size_t n =
                ZSTD_decompressDCtx(thread_dctx(), dst.data(), dst.size(), src.data(), src.size());
            if (ZSTD_isError(n))
                return model_err(std::string("corrupt Zstd chunk: ") + ZSTD_getErrorName(n));
            if (n != dst.size())
                return model_err("Zstd chunk decoded to unexpected size");
            return {};
        }
    }
    return Error(ErrorCode::Unsupported, "unknown codec");
}

std::string_view lz4_version() noexcept {
    static const std::string v = LZ4_versionString();
    return v;
}
std::string_view zstd_version() noexcept {
    static const std::string v = ZSTD_versionString();
    return v;
}

}  // namespace hcs::storage
