#pragma once
// Internal: uniform read access to the tensors of a safetensors or GGUF file for the merge engine.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/storage/file_source.hpp"

namespace hcs::merge::detail {

enum class DType { F32, F16, BF16, Other };

inline bool is_float(DType d) noexcept {
    return d != DType::Other;
}
inline std::uint64_t elem_bytes(DType d) noexcept {
    return d == DType::F32 ? 4 : (d == DType::Other ? 0 : 2);
}

struct Tensor {
    std::string name;
    DType dtype = DType::Other;
    std::string type_name;             // "F32", "F16", "BF16", "I64", "ggml_type_12", ...
    std::vector<std::uint64_t> shape;  // row-major (GGUF dims are reversed into this order)
    std::uint64_t file_offset = 0;     // absolute offset of the first data byte
    std::uint64_t bytes = 0;
    std::uint64_t elements = 0;
    std::uint64_t rel_offset = 0;  // offset relative to the data section (safetensors begin / GGUF offset)
};

class TensorFile {
public:
    enum class Format { Safetensors, Gguf };

    static Result<std::unique_ptr<TensorFile>> open(const std::filesystem::path& path);

    Format format() const noexcept { return format_; }
    const char* format_name() const noexcept { return format_ == Format::Gguf ? "gguf" : "safetensors"; }
    const std::filesystem::path& path() const noexcept { return path_; }
    std::uint64_t file_size() const noexcept { return size_; }
    std::uint64_t data_offset() const noexcept { return data_offset_; }
    const std::vector<Tensor>& tensors() const noexcept { return tensors_; }  // sorted by file_offset
    const std::map<std::string, std::string>& metadata() const noexcept { return metadata_; }
    const Tensor* find(const std::string& name) const noexcept;

    // Reads `count` elements starting at element `first`, converted to float. Float dtypes only.
    Result<void> read_floats(const Tensor& t, std::uint64_t first, std::uint64_t count, float* out) const;
    // Raw bytes of the file.
    Result<void> read_bytes(std::uint64_t offset, void* dst, std::size_t n) const;

private:
    TensorFile() = default;
    Format format_ = Format::Safetensors;
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    std::uint64_t data_offset_ = 0;
    std::vector<Tensor> tensors_;
    std::map<std::string, std::string> metadata_;
    std::unique_ptr<storage::FileSource> src_;
};

}  // namespace hcs::merge::detail
