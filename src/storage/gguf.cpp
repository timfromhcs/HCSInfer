#include "hcs/storage/gguf.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#include "hcs/checked_math.hpp"

namespace hcs::storage {
namespace {

Error bad(std::string m) {
    return Error(ErrorCode::ModelError, "GGUF: " + std::move(m));
}

// Buffered forward cursor with absolute bounds checking against the real source size.
class Cursor {
public:
    explicit Cursor(const ByteSource& s) : src_(s), size_(s.size()), buf_(kWindow) {}

    std::uint64_t pos() const { return pos_; }
    std::uint64_t remaining() const { return size_ - pos_; }

    Result<void> read(void* dst, std::size_t n) {
        if (n > remaining())
            return bad("unexpected end of file (truncated)");
        auto* d = static_cast<std::uint8_t*>(dst);
        while (n) {
            if (pos_ < win_begin_ || pos_ >= win_begin_ + win_len_) {
                if (auto r = refill(); !r)
                    return r;
            }
            const std::size_t off = static_cast<std::size_t>(pos_ - win_begin_);
            const std::size_t take = std::min<std::size_t>(n, win_len_ - off);
            std::memcpy(d, buf_.data() + off, take);
            d += take;
            pos_ += take;
            n -= take;
        }
        return {};
    }

    Result<void> skip(std::uint64_t n) {
        if (n > remaining())
            return bad("skip beyond end of file (truncated)");
        pos_ += n;
        return {};
    }

    template <class T>
    Result<T> get() {
        std::uint8_t b[8] = {};
        static_assert(sizeof(T) <= 8);
        if (auto r = read(b, sizeof(T)); !r)
            return r.error();
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) v |= std::uint64_t(b[i]) << (8 * i);
        return static_cast<T>(v);
    }

private:
    static constexpr std::size_t kWindow = 1u << 20;
    Result<void> refill() {
        const std::uint64_t n = std::min<std::uint64_t>(kWindow, size_ - pos_);
        if (auto r = src_.read_at(pos_, buf_.data(), static_cast<std::size_t>(n)); !r)
            return r;
        win_begin_ = pos_;
        win_len_ = static_cast<std::size_t>(n);
        return {};
    }

    const ByteSource& src_;
    std::uint64_t size_;
    std::uint64_t pos_ = 0;
    std::vector<std::uint8_t> buf_;
    std::uint64_t win_begin_ = 0;
    std::size_t win_len_ = 0;
};

// Size in bytes of a fixed-size GGUF value type, 0 if not fixed-size.
std::uint64_t fixed_value_size(std::uint32_t t) {
    switch (t) {
        case 0:
        case 1:
        case 7:
            return 1;
        case 2:
        case 3:
            return 2;
        case 4:
        case 5:
        case 6:
            return 4;
        case 10:
        case 11:
        case 12:
            return 8;
        default:
            return 0;
    }
}

struct Budget {
    std::uint64_t elements = 1ull << 28;  // total array elements walked in one file
};

Result<std::string> read_string(Cursor& c, std::uint64_t max_len, const char* what) {
    auto len = c.get<std::uint64_t>();
    if (!len)
        return len.error();
    if (len.value() > max_len)
        return bad(std::string(what) + " length exceeds the cap");
    if (len.value() > c.remaining())
        return bad(std::string(what) + " length exceeds the file (truncated)");
    std::string s(static_cast<std::size_t>(len.value()), '\0');
    if (auto r = c.read(s.data(), s.size()); !r)
        return r.error();
    return s;
}

Result<void> skip_string(Cursor& c) {
    auto len = c.get<std::uint64_t>();
    if (!len)
        return len.error();
    if (len.value() > c.remaining())
        return bad("string length exceeds the file (truncated)");
    return c.skip(len.value());
}

Result<void> skip_value(Cursor& c, std::uint32_t type, int depth, Budget& budget) {
    if (const auto fs = fixed_value_size(type))
        return c.skip(fs);
    if (type == 8)
        return skip_string(c);
    if (type != 9)
        return bad("unknown metadata value type " + std::to_string(type));
    if (depth >= kGgufMaxArrayDepth)
        return bad("metadata arrays nested too deeply");
    auto et = c.get<std::uint32_t>();
    if (!et)
        return et.error();
    auto count = c.get<std::uint64_t>();
    if (!count)
        return count.error();
    if (count.value() > budget.elements)
        return bad("metadata array element count exceeds the cap");
    budget.elements -= count.value();
    if (const auto fs = fixed_value_size(et.value())) {
        const auto bytes = checked_mul(count.value(), fs);
        if (!bytes || *bytes > c.remaining())
            return bad("metadata array exceeds the file (truncated)");
        return c.skip(*bytes);
    }
    if (et.value() == 8 || et.value() == 9) {
        // Each element needs at least 8 bytes (string length / array header 12).
        const auto min_bytes = checked_mul(count.value(), 8);
        if (!min_bytes || *min_bytes > c.remaining())
            return bad("metadata array exceeds the file (truncated)");
        for (std::uint64_t i = 0; i < count.value(); ++i) {
            if (auto r = skip_value(c, et.value(), depth + 1, budget); !r)
                return r;
        }
        return {};
    }
    return bad("unknown metadata array element type " + std::to_string(et.value()));
}

}  // namespace

bool ggml_type_layout(std::uint32_t t, std::uint64_t& be, std::uint64_t& bb) noexcept {
    struct L {
        std::uint32_t type;
        std::uint64_t elems, bytes;
    };
    static constexpr L table[] = {
        {0, 1, 4},      {1, 1, 2},      {2, 32, 18},   {3, 32, 20},    {6, 32, 22},    {7, 32, 24},
        {8, 32, 34},    {9, 32, 36},    {10, 256, 84}, {11, 256, 110}, {12, 256, 144}, {13, 256, 176},
        {14, 256, 210}, {15, 256, 292}, {16, 256, 66}, {17, 256, 74},  {18, 256, 98},  {19, 256, 50},
        {20, 32, 18},   {21, 256, 110}, {22, 256, 82}, {23, 256, 136}, {24, 1, 1},     {25, 1, 2},
        {26, 1, 4},     {27, 1, 8},     {28, 1, 8},    {29, 256, 56},  {30, 1, 2},     {34, 256, 54},
        {35, 256, 66},  {39, 32, 17}};
    for (const auto& l : table)
        if (l.type == t) {
            be = l.elems;
            bb = l.bytes;
            return true;
        }
    return false;
}

Result<GgufInfo> parse_gguf(const ByteSource& source) {
    Cursor c(source);
    GgufInfo info;
    info.file_size = source.size();

    char magic[4];
    if (source.size() < 24)
        return bad("file too small for a GGUF header (truncated)");
    if (auto r = c.read(magic, 4); !r)
        return r.error();
    if (std::memcmp(magic, "GGUF", 4) != 0)
        return bad("bad magic");
    auto ver = c.get<std::uint32_t>();
    if (!ver)
        return ver.error();
    info.version = ver.value();
    if (info.version < 2)
        return Error(ErrorCode::Unsupported,
                     "GGUF: version " + std::to_string(info.version) + " is not supported (need 2 or 3)");
    if (info.version > 3)
        return Error(ErrorCode::Unsupported,
                     "GGUF: version " + std::to_string(info.version) + " is newer than this reader");
    auto tc = c.get<std::uint64_t>();
    if (!tc)
        return tc.error();
    auto kc = c.get<std::uint64_t>();
    if (!kc)
        return kc.error();
    info.tensor_count = tc.value();
    info.kv_count = kc.value();
    if (info.tensor_count > kGgufMaxTensors)
        return bad("tensor count exceeds the cap");
    if (info.kv_count > kGgufMaxKv)
        return bad("metadata count exceeds the cap");
    // Each tensor info needs >= 8 (name len) + 4 + 8 (one dim) + 4 + 8 bytes; each KV >= 8 + 4 bytes.
    if (info.tensor_count * 32 > c.remaining())
        return bad("tensor count exceeds the file size (truncated)");
    if (info.kv_count * 12 > c.remaining())
        return bad("metadata count exceeds the file size (truncated)");

    Budget budget;
    std::set<std::string> seen_keys;
    for (std::uint64_t i = 0; i < info.kv_count; ++i) {
        auto key = read_string(c, kGgufMaxKeyLen, "metadata key");
        if (!key)
            return key.error();
        auto type = c.get<std::uint32_t>();
        if (!type)
            return type.error();
        const std::string& k = key.value();
        const bool keep = (k == "general.architecture" || k == "general.name");
        if (keep && type.value() == 8) {
            auto sv = read_string(c, kGgufMaxKeptStringLen, "metadata string");
            if (!sv)
                return sv.error();
            (k == "general.architecture" ? info.architecture : info.name) = std::move(sv).value();
        } else if (k == "general.alignment" && type.value() == 4) {
            auto v = c.get<std::uint32_t>();
            if (!v)
                return v.error();
            info.alignment = v.value();
        } else if (k == "general.file_type" && type.value() == 4) {
            auto v = c.get<std::uint32_t>();
            if (!v)
                return v.error();
            info.file_type = v.value();
        } else {
            if (auto r = skip_value(c, type.value(), 0, budget); !r)
                return r.error();
        }
    }
    if (info.alignment == 0 || (info.alignment & (info.alignment - 1)) != 0 || info.alignment > (1u << 20))
        return bad("invalid general.alignment");

    info.tensors.reserve(static_cast<std::size_t>(info.tensor_count));  // bounded by file size check above
    for (std::uint64_t i = 0; i < info.tensor_count; ++i) {
        GgufTensorInfo t;
        auto name = read_string(c, kGgufMaxTensorNameLen, "tensor name");
        if (!name)
            return name.error();
        t.name = std::move(name).value();
        auto nd = c.get<std::uint32_t>();
        if (!nd)
            return nd.error();
        if (nd.value() == 0 || nd.value() > 4)
            return bad("tensor '" + t.name + "' has an invalid dimension count");
        t.n_dims = nd.value();
        for (std::uint32_t d = 0; d < t.n_dims; ++d) {
            auto dim = c.get<std::uint64_t>();
            if (!dim)
                return dim.error();
            if (dim.value() == 0)
                return bad("tensor '" + t.name + "' has a zero dimension");
            t.dims[d] = dim.value();
        }
        auto ty = c.get<std::uint32_t>();
        if (!ty)
            return ty.error();
        t.type = ty.value();
        auto off = c.get<std::uint64_t>();
        if (!off)
            return off.error();
        t.offset = off.value();
        if (t.offset % info.alignment != 0)
            return bad("tensor '" + t.name + "' offset is not aligned");
        std::uint64_t be = 0, bb = 0;
        if (ggml_type_layout(t.type, be, bb)) {
            if (t.dims[0] % be != 0)
                return bad("tensor '" + t.name + "' row size is not a multiple of the block size");
            std::uint64_t bytes = t.dims[0] / be;
            auto b0 = checked_mul(bytes, bb);
            if (!b0)
                return bad("tensor '" + t.name + "' size overflows");
            std::uint64_t total = *b0;
            for (std::uint32_t d = 1; d < t.n_dims; ++d) {
                auto m = checked_mul(total, t.dims[d]);
                if (!m)
                    return bad("tensor '" + t.name + "' size overflows");
                total = *m;
            }
            t.byte_size = total;
            t.size_known = true;
        }
        info.tensors.push_back(std::move(t));
    }

    const std::uint64_t pos = c.pos();
    const auto aligned = checked_add(pos, info.alignment - 1);
    if (!aligned)
        return bad("data offset overflows");
    info.data_offset = *aligned & ~(info.alignment - 1);
    if (info.data_offset > info.file_size)
        return bad("tensor data section missing (truncated)");
    const std::uint64_t data_size = info.file_size - info.data_offset;
    for (const auto& t : info.tensors) {
        if (t.offset > data_size)
            return bad("tensor '" + t.name + "' lies outside the data section (truncated)");
        if (t.size_known && !range_within(t.offset, t.byte_size, data_size))
            return bad("tensor '" + t.name + "' extends past the end of the file (truncated)");
    }
    return info;
}

}  // namespace hcs::storage
