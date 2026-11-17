#pragma once
// Safe, bounded GGUF header parser. The file is untrusted: every count, length and offset is
// checked against caps and the real source size before use. Nothing in the file is executed and
// no allocation is sized by an unvalidated field. Tensor payloads are never read here.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/storage/file_source.hpp"

namespace hcs::storage {

inline constexpr std::uint64_t kGgufMaxTensors = 1ull << 20;
inline constexpr std::uint64_t kGgufMaxKv = 1ull << 20;
inline constexpr std::uint64_t kGgufMaxKeyLen = 65535;
inline constexpr std::uint64_t kGgufMaxTensorNameLen = 1024;
inline constexpr std::uint64_t kGgufMaxKeptStringLen = 1ull << 20;  // kept (whitelisted) string values
inline constexpr int kGgufMaxArrayDepth = 4;

struct GgufTensorInfo {
    std::string name;
    std::uint32_t type = 0;
    std::uint32_t n_dims = 0;
    std::uint64_t dims[4] = {1, 1, 1, 1};
    std::uint64_t offset = 0;     // relative to data_offset
    std::uint64_t byte_size = 0;  // 0 when the type is not in the known-size table
    bool size_known = false;
};

struct GgufInfo {
    std::uint32_t version = 0;
    std::uint64_t tensor_count = 0;
    std::uint64_t kv_count = 0;
    std::uint64_t alignment = 32;
    std::uint64_t data_offset = 0;
    std::uint64_t file_size = 0;
    std::string architecture;                // general.architecture ("" if absent)
    std::string name;                        // general.name ("" if absent)
    std::optional<std::uint32_t> file_type;  // general.file_type
    std::vector<GgufTensorInfo> tensors;
};

// Accepts GGUF versions 2 and 3 (v1 uses 32-bit counts and is rejected as Unsupported).
Result<GgufInfo> parse_gguf(const ByteSource& source);

// Block size / bytes-per-block for a ggml tensor type id; false if unknown to this table.
bool ggml_type_layout(std::uint32_t type, std::uint64_t& block_elems, std::uint64_t& block_bytes) noexcept;

}  // namespace hcs::storage
