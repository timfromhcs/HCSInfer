#pragma once
// Bounded safetensors header reader (AGENTS.md 19.3: model files are untrusted input).
//
// Layout: u64 little-endian header length N, N bytes of JSON ({"name": {dtype, shape, data_offsets:[b,e]},
// "__metadata__": {...}}), then the raw tensor bytes. This module reads and VALIDATES the header only; it
// never maps or decodes tensor data. Every size is range-checked against the real file size, the header is
// capped, shapes/offsets use checked arithmetic and tensor ranges must not overlap, so a truncated or hostile
// file yields an error instead of a crash, a huge allocation or an out-of-bounds read.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/result.hpp"

namespace hcs {

struct SafetensorsTensor {
    std::string name;
    std::string dtype;  // "F32", "F16", "BF16", ...
    std::vector<std::uint64_t> shape;
    std::uint64_t begin = 0;  // byte range relative to SafetensorsHeader::data_offset
    std::uint64_t end = 0;
    std::uint64_t element_count() const noexcept;  // product of shape (1 for a scalar)
    std::uint64_t byte_size() const noexcept { return end - begin; }
};

struct SafetensorsHeader {
    std::vector<SafetensorsTensor> tensors;  // sorted by `begin`
    std::map<std::string, std::string> metadata;
    std::uint64_t data_offset = 0;  // 8 + header length
    std::uint64_t file_size = 0;
    const SafetensorsTensor* find(std::string_view name) const noexcept;
};

// Bytes per element for a safetensors dtype string, 0 when unknown.
std::uint64_t safetensors_dtype_size(std::string_view dtype) noexcept;

// Parses the header of an in-memory file prefix. `file_size` is the real size of the whole file.
Result<SafetensorsHeader> parse_safetensors_header(const std::uint8_t* prefix, std::size_t prefix_len,
                                                   std::uint64_t file_size,
                                                   std::uint64_t max_header_bytes = 64u << 20);

// Reads the header from disk (only the header bytes are read).
Result<SafetensorsHeader> read_safetensors_header(const std::filesystem::path& path,
                                                  std::uint64_t max_header_bytes = 64u << 20);

}  // namespace hcs
