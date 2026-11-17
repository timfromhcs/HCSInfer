#include "hcs/gguf_probe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <string_view>
#include <vector>

namespace hcs {
namespace {

// GGUF metadata value types (ggml/include/gguf.h). Kept local: this header must not leak ggml.
enum : std::uint32_t {
    kU8 = 0,
    kI8 = 1,
    kU16 = 2,
    kI16 = 3,
    kU32 = 4,
    kI32 = 5,
    kF32 = 6,
    kBool = 7,
    kString = 8,
    kArray = 9,
    kU64 = 10,
    kI64 = 11,
    kF64 = 12,
};

constexpr std::uint64_t kGgufMagic = 0x46554747;  // "GGUF" little-endian
constexpr std::uint32_t kMaxDims = 8;
constexpr std::uint64_t kMaxKeyLen = 4096;
constexpr std::uint64_t kMaxElementName = 4096;
constexpr std::uint64_t kMaxArchitectureLen = 256;
constexpr std::uint32_t kMaxArrayDepth = 4;
constexpr std::uint64_t kMaxArrayElements = 1ull << 24;
constexpr std::uint64_t kMaxKvPairs = 100000;

// Returns the byte size of a fixed-size value type, 0 for string/array, -1 for an unknown type.
int fixed_size(std::uint32_t t) {
    switch (t) {
        case kU8:
        case kI8:
        case kBool: return 1;
        case kU16:
        case kI16: return 2;
        case kU32:
        case kI32:
        case kF32: return 4;
        case kU64:
        case kI64:
        case kF64: return 8;
        case kString:
        case kArray: return 0;
        default: return -1;
    }
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> b) : b_(b) {}

    std::size_t remaining() const { return b_.size() - off_; }
    bool ok() const { return ok_; }
    const std::string& reason() const { return reason_; }
    bool fail(const char* why) {
        if (ok_) {
            ok_ = false;
            reason_ = why;
        }
        return false;
    }

    // Reads a little-endian unsigned integer of n bytes (n <= 8).
    std::uint64_t le(std::uint32_t n) {
        if (n > 8) {
            fail("invalid integer width");
            return 0;
        }
        if (n > remaining()) {
            fail("truncated value");
            return 0;
        }
        std::uint64_t v = 0;
        for (std::uint32_t i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(b_[off_ + i]) << (8 * i);
        off_ += n;
        return v;
    }
    bool skip(std::uint64_t n) {
        if (!ok()) return false;
        if (n > remaining()) return fail("truncated payload");
        off_ += static_cast<std::size_t>(n);
        return true;
    }
    // Length-prefixed string with a caller-supplied cap on the declared length.
    bool str(std::uint64_t cap, std::string& out) {
        const std::uint64_t len = le(8);
        if (!ok()) return false;
        if (len > cap) return fail("string length exceeds the accepted maximum");
        if (len > remaining()) return fail("truncated string");
        out.assign(reinterpret_cast<const char*>(b_.data() + off_), static_cast<std::size_t>(len));
        off_ += static_cast<std::size_t>(len);
        return true;
    }

private:
    std::span<const std::uint8_t> b_;
    std::size_t off_ = 0;
    bool ok_ = true;
    std::string reason_;
};

bool skip_value(Reader& r, std::uint32_t type, std::uint32_t depth);

bool skip_array(Reader& r, std::uint32_t depth) {
    const std::uint64_t elem = r.le(4);
    const std::uint64_t count = r.le(8);
    if (!r.ok()) return false;
    if (depth >= kMaxArrayDepth) return r.fail("array nesting is too deep");
    const int fs = fixed_size(static_cast<std::uint32_t>(elem));
    if (fs > 0) {
        const std::uint64_t bytes = count * static_cast<std::uint64_t>(fs);
        if (count != 0 && bytes / static_cast<std::uint64_t>(fs) != count)
            return r.fail("array size overflows");
        if (bytes > r.remaining()) return r.fail("array exceeds the buffer");
        return r.skip(bytes);
    }
    if (fs < 0) return r.fail("unknown array element type");
    if (count > kMaxArrayElements) return r.fail("array has too many elements");
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!skip_value(r, static_cast<std::uint32_t>(elem), depth + 1)) return false;
    }
    return true;
}

bool skip_value(Reader& r, std::uint32_t type, std::uint32_t depth) {
    const int fs = fixed_size(type);
    if (fs > 0) return r.skip(static_cast<std::uint64_t>(fs));
    if (type == kString) {
        std::string ignored;
        return r.str(kMaxElementName * 64, ignored);  // bound a single metadata string to 256 KiB
    }
    if (type == kArray) return skip_array(r, depth);
    return r.fail("unknown metadata value type");
}

bool looks_like_diffusion_tensor(std::string_view n) {
    static constexpr std::array<std::string_view, 5> kPrefixes{"model.diffusion_model", "cond_stage_model",
                                                               "first_stage_model", "alphas_cumprod", "vae."};
    for (const auto p : kPrefixes)
        if (n.rfind(p, 0) == 0) return true;
    return false;
}

bool looks_like_text_tensor(std::string_view n) {
    static constexpr std::array<std::string_view, 4> kPrefixes{"blk.", "token_embd", "output.", "model.embed"};
    for (const auto p : kPrefixes)
        if (n.rfind(p, 0) == 0) return true;
    return false;
}

bool architecture_is_diffusion(std::string_view a) {
    std::string low(a);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return low.find("diffusion") != std::string::npos || low.rfind("stable-diffusion", 0) == 0 ||
           low.rfind("sdxl", 0) == 0 || low.rfind("flux", 0) == 0;
}

}  // namespace

GgufProbeResult probe_gguf(std::span<const std::uint8_t> bytes, std::size_t max_tensor_names) {
    GgufProbeResult out;
    if (bytes.size() < 8) {
        out.reason = "file too small for a GGUF header";
        return out;
    }
    Reader r(bytes);
    if (r.le(4) != kGgufMagic) {
        out.reason = r.ok() ? "invalid GGUF magic" : r.reason();
        return out;
    }
    out.version = static_cast<std::uint32_t>(r.le(4));
    if (!r.ok()) {
        out.reason = r.reason();
        return out;
    }
    if (out.version < 1 || out.version > 3) {
        out.reason = "unsupported GGUF version " + std::to_string(out.version);
        return out;
    }
    out.valid = true;
    if (out.version == 1) {
        // The pinned llama.cpp refuses GGUFv1, and its 32-bit header counts are not worth emulating.
        out.kind = ModelKind::Text;
        out.reason = "GGUFv1 is not parsed and not supported by the pinned engines";
        return out;
    }

    out.tensor_count = r.le(8);
    out.kv_count = r.le(8);
    if (!r.ok()) {
        out.reason = r.reason();
        out.valid = false;
        return out;
    }

    bool saw_diffusion = false, saw_text = false;
    const std::uint64_t kv_walk = std::min<std::uint64_t>(out.kv_count, kMaxKvPairs);
    for (std::uint64_t i = 0; i < kv_walk; ++i) {
        std::string key;
        if (!r.str(kMaxKeyLen, key)) {
            out.reason = r.reason();
            return out;
        }
        const std::uint32_t type = static_cast<std::uint32_t>(r.le(4));
        if (!r.ok()) {
            out.reason = r.reason();
            return out;
        }
        if (key == "general.architecture" && type == kString) {
            if (!r.str(kMaxArchitectureLen, out.architecture)) {
                out.reason = r.reason();
                return out;
            }
            continue;
        }
        if (!skip_value(r, type, 0)) {
            out.reason = r.reason();
            return out;
        }
    }

    const std::uint64_t walk = std::min<std::uint64_t>(out.tensor_count, max_tensor_names);
    for (std::uint64_t i = 0; i < walk; ++i) {
        std::string name;
        if (!r.str(kMaxElementName, name)) {
            out.reason = r.reason();
            return out;
        }
        ++out.tensor_names_read;
        if (!saw_diffusion && looks_like_diffusion_tensor(name)) saw_diffusion = true;
        if (!saw_text && looks_like_text_tensor(name)) saw_text = true;
        const std::uint64_t ndims = r.le(4);
        if (!r.ok()) {
            out.reason = r.reason();
            return out;
        }
        if (ndims > kMaxDims) {
            out.reason = "tensor has too many dimensions";
            out.valid = false;
            return out;
        }
        if (!r.skip(ndims * 8)) {
            out.reason = r.reason();
            return out;
        }
        if (!r.skip(4)) {  // ggml tensor type: not interpreted here, only skipped
            out.reason = r.reason();
            return out;
        }
        if (!r.skip(8)) {  // data offset
            out.reason = r.reason();
            return out;
        }
    }

    out.complete = out.kv_count <= kMaxKvPairs && out.tensor_names_read == out.tensor_count;
    if (architecture_is_diffusion(out.architecture)) saw_diffusion = true;
    if (saw_diffusion) {
        out.kind = ModelKind::Image;
        if (out.architecture.empty()) out.architecture = "stable-diffusion";
    } else if (!out.architecture.empty() || saw_text) {
        out.kind = ModelKind::Text;
    }
    return out;
}

GgufProbeResult probe_gguf_file(const std::filesystem::path& path, std::size_t max_bytes) {
    GgufProbeResult out;
    if (max_bytes < 24) {
        out.reason = "probe limit is too small for a GGUF header";
        return out;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.reason = "cannot open file";
        return out;
    }
    std::vector<std::uint8_t> buf(max_bytes);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    buf.resize(got);
    if (got == 0) {
        out.reason = "file is empty";
        return out;
    }
    return probe_gguf(std::span<const std::uint8_t>(buf.data(), buf.size()));
}

}  // namespace hcs
