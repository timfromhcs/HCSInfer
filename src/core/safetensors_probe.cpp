#include "hcs/safetensors.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

#include "hcs/checked_math.hpp"

namespace hcs {

std::uint64_t SafetensorsTensor::element_count() const noexcept {
    std::uint64_t n = 1;
    for (auto d : shape) {
        auto m = checked_mul(n, d);
        if (!m)
            return 0;
        n = *m;
    }
    return n;
}

const SafetensorsTensor* SafetensorsHeader::find(std::string_view name) const noexcept {
    for (const auto& t : tensors)
        if (t.name == name)
            return &t;
    return nullptr;
}

std::uint64_t safetensors_dtype_size(std::string_view d) noexcept {
    if (d == "F64" || d == "I64" || d == "U64")
        return 8;
    if (d == "F32" || d == "I32" || d == "U32")
        return 4;
    if (d == "F16" || d == "BF16" || d == "I16" || d == "U16")
        return 2;
    if (d == "I8" || d == "U8" || d == "BOOL" || d == "F8_E4M3" || d == "F8_E5M2")
        return 1;
    return 0;
}

namespace {
Error bad(const std::string& m) {
    return Error(ErrorCode::ModelError, "invalid safetensors file: " + m);
}
}  // namespace

Result<SafetensorsHeader> parse_safetensors_header(const std::uint8_t* p, std::size_t len,
                                                   std::uint64_t file_size, std::uint64_t max_header) {
    if (len < 8 || file_size < 8)
        return bad("shorter than the 8-byte length prefix");
    std::uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | p[i];
    if (n == 0 || n > max_header)
        return bad("header length " + std::to_string(n) + " is outside 1.." + std::to_string(max_header));
    if (!range_within(8, n, file_size))
        return bad("header length " + std::to_string(n) + " exceeds the file size");
    if (!range_within(8, n, len))
        return bad("header bytes were not supplied");
    const std::uint64_t data_offset = 8 + n;
    const std::uint64_t data_size = file_size - data_offset;
    nlohmann::json j = nlohmann::json::parse(p + 8, p + 8 + n, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return bad("header is not a JSON object");
    SafetensorsHeader h;
    h.data_offset = data_offset;
    h.file_size = file_size;
    constexpr std::size_t kMaxTensors = 1u << 20;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "__metadata__") {
            if (!it.value().is_object())
                return bad("__metadata__ is not an object");
            for (auto m = it.value().begin(); m != it.value().end(); ++m)
                if (m.value().is_string())
                    h.metadata[m.key()] = m.value().get<std::string>();
            continue;
        }
        if (h.tensors.size() >= kMaxTensors)
            return bad("too many tensors");
        const auto& v = it.value();
        if (!v.is_object() || !v.contains("dtype") || !v["dtype"].is_string() || !v.contains("shape") ||
            !v["shape"].is_array() || !v.contains("data_offsets") || !v["data_offsets"].is_array() ||
            v["data_offsets"].size() != 2)
            return bad("tensor '" + it.key() + "' is malformed");
        SafetensorsTensor t;
        t.name = it.key();
        t.dtype = v["dtype"].get<std::string>();
        const std::uint64_t esz = safetensors_dtype_size(t.dtype);
        if (esz == 0)
            return bad("tensor '" + t.name + "' has unknown dtype '" + t.dtype + "'");
        std::uint64_t elems = 1;
        for (const auto& d : v["shape"]) {
            if (!d.is_number_unsigned())
                return bad("tensor '" + t.name + "' has a non-integer dimension");
            t.shape.push_back(d.get<std::uint64_t>());
            auto m = checked_mul(elems, t.shape.back());
            if (!m)
                return bad("tensor '" + t.name + "' element count overflows");
            elems = *m;
        }
        const auto& off = v["data_offsets"];
        if (!off[0].is_number_unsigned() || !off[1].is_number_unsigned())
            return bad("tensor '" + t.name + "' has non-integer offsets");
        t.begin = off[0].get<std::uint64_t>();
        t.end = off[1].get<std::uint64_t>();
        if (t.end < t.begin || t.end > data_size)
            return bad("tensor '" + t.name + "' data range lies outside the file");
        auto bytes = checked_mul(elems, esz);
        if (!bytes || *bytes != t.end - t.begin)
            return bad("tensor '" + t.name + "' byte size does not match shape and dtype");
        h.tensors.push_back(std::move(t));
    }
    std::sort(h.tensors.begin(), h.tensors.end(),
              [](const SafetensorsTensor& a, const SafetensorsTensor& b) { return a.begin < b.begin; });
    for (std::size_t i = 1; i < h.tensors.size(); ++i)
        if (h.tensors[i].begin < h.tensors[i - 1].end && h.tensors[i].end != h.tensors[i].begin)
            return bad("tensors '" + h.tensors[i - 1].name + "' and '" + h.tensors[i].name + "' overlap");
    return h;
}

Result<SafetensorsHeader> read_safetensors_header(const std::filesystem::path& path,
                                                  std::uint64_t max_header) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return Error(ErrorCode::NotFound, "cannot stat '" + path.string() + "': " + ec.message());
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Error(ErrorCode::IoError, "cannot open '" + path.string() + "'");
    std::uint8_t lenbuf[8] = {};
    in.read(reinterpret_cast<char*>(lenbuf), 8);
    if (in.gcount() != 8)
        return bad("shorter than the 8-byte length prefix");
    std::uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | lenbuf[i];
    if (n == 0 || n > max_header || !range_within(8, n, size))
        return bad("header length " + std::to_string(n) + " is invalid for a " + std::to_string(size) +
                   "-byte file");
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(8 + n));
    std::copy(lenbuf, lenbuf + 8, buf.begin());
    in.read(reinterpret_cast<char*>(buf.data() + 8), static_cast<std::streamsize>(n));
    if (static_cast<std::uint64_t>(in.gcount()) != n)
        return Error(ErrorCode::IoError, "short read of the safetensors header");
    return parse_safetensors_header(buf.data(), buf.size(), size, max_header);
}

}  // namespace hcs
