#include "tensor_file.hpp"

#include <algorithm>
#include <cstring>

#include "hcs/checked_math.hpp"
#include "hcs/merge.hpp"
#include "hcs/safetensors.hpp"
#include "hcs/storage/gguf.hpp"

namespace hcs::merge::detail {

namespace {
DType gguf_dtype(std::uint32_t t, std::string& name) {
    switch (t) {
        case 0:
            name = "F32";
            return DType::F32;
        case 1:
            name = "F16";
            return DType::F16;
        case 30:
            name = "BF16";
            return DType::BF16;
        default:
            name = "ggml_type_" + std::to_string(t);
            return DType::Other;
    }
}
DType st_dtype(const std::string& d) {
    if (d == "F32")
        return DType::F32;
    if (d == "F16")
        return DType::F16;
    if (d == "BF16")
        return DType::BF16;
    return DType::Other;
}
}  // namespace

Result<std::unique_ptr<TensorFile>> TensorFile::open(const std::filesystem::path& path) {
    auto src = storage::FileSource::open(path, true);
    if (!src)
        return src.error();
    std::unique_ptr<TensorFile> f(new TensorFile());
    f->path_ = path;
    f->size_ = src.value()->size();
    std::uint8_t magic[4] = {};
    if (f->size_ >= 4)
        if (auto r = src.value()->read_at(0, magic, 4); !r)
            return r.error();
    if (std::memcmp(magic, "GGUF", 4) == 0) {
        auto info = storage::parse_gguf(*src.value());
        if (!info)
            return info.error();
        f->format_ = Format::Gguf;
        f->data_offset_ = info.value().data_offset;
        for (const auto& t : info.value().tensors) {
            Tensor x;
            x.name = t.name;
            x.dtype = gguf_dtype(t.type, x.type_name);
            for (std::uint32_t i = t.n_dims; i > 0; --i) x.shape.push_back(t.dims[i - 1]);
            x.elements = 1;
            for (auto d : x.shape) {
                auto m = checked_mul(x.elements, d);
                if (!m)
                    return Error(ErrorCode::ModelError,
                                 "GGUF tensor '" + t.name + "' element count overflows");
                x.elements = *m;
            }
            x.rel_offset = t.offset;
            x.file_offset = f->data_offset_ + t.offset;
            if (is_float(x.dtype))
                x.bytes = x.elements * elem_bytes(x.dtype);
            else if (t.size_known)
                x.bytes = t.byte_size;
            else
                return Error(
                    ErrorCode::Unsupported,
                    "GGUF tensor '" + t.name + "' has a type this build cannot size (" + x.type_name + ")");
            if (!range_within(x.file_offset, x.bytes, f->size_))
                return Error(ErrorCode::ModelError, "GGUF tensor '" + t.name + "' lies outside the file");
            f->tensors_.push_back(std::move(x));
        }
    } else {
        std::vector<std::uint8_t> prefix;
        // The header is read through the safetensors parser (bounded, validated).
        auto hdr = read_safetensors_header(path);
        if (!hdr)
            return hdr.error();
        f->format_ = Format::Safetensors;
        f->data_offset_ = hdr.value().data_offset;
        f->metadata_ = hdr.value().metadata;
        for (const auto& t : hdr.value().tensors) {
            Tensor x;
            x.name = t.name;
            x.type_name = t.dtype;
            x.dtype = st_dtype(t.dtype);
            x.shape = t.shape;
            x.elements = t.element_count();
            x.rel_offset = t.begin;
            x.file_offset = f->data_offset_ + t.begin;
            x.bytes = t.byte_size();
            f->tensors_.push_back(std::move(x));
        }
    }
    std::sort(f->tensors_.begin(), f->tensors_.end(),
              [](const Tensor& a, const Tensor& b) { return a.file_offset < b.file_offset; });
    for (std::size_t i = 1; i < f->tensors_.size(); ++i)
        if (f->tensors_[i].file_offset < f->tensors_[i - 1].file_offset + f->tensors_[i - 1].bytes)
            return Error(ErrorCode::ModelError, "tensors '" + f->tensors_[i - 1].name + "' and '" +
                                                    f->tensors_[i].name + "' overlap");
    {
        std::map<std::string, int> seen;
        for (const auto& t : f->tensors_)
            if (seen[t.name]++)
                return Error(ErrorCode::ModelError, "duplicate tensor name '" + t.name + "'");
    }
    f->src_ = std::move(src.value());
    return f;
}

const Tensor* TensorFile::find(const std::string& name) const noexcept {
    for (const auto& t : tensors_)
        if (t.name == name)
            return &t;
    return nullptr;
}

Result<void> TensorFile::read_bytes(std::uint64_t offset, void* dst, std::size_t n) const {
    if (n == 0)
        return {};
    return src_->read_at(offset, dst, n);
}

Result<void> TensorFile::read_floats(const Tensor& t, std::uint64_t first, std::uint64_t count,
                                     float* out) const {
    if (!is_float(t.dtype))
        return make_error(ErrorCode::Unsupported,
                          "tensor '" + t.name + "' has type " + t.type_name + ", not a float type");
    if (!range_within(first, count, t.elements))
        return make_error(ErrorCode::InvalidArgument, "read beyond the end of tensor '" + t.name + "'");
    if (count == 0)
        return {};
    const std::uint64_t es = elem_bytes(t.dtype);
    const std::uint64_t off = t.file_offset + first * es;
    if (t.dtype == DType::F32) {
        if (auto r = src_->read_at(off, out, static_cast<std::size_t>(count * 4)); !r)
            return r;
        return {};
    }
    std::vector<std::uint16_t> raw(static_cast<std::size_t>(count));
    if (auto r = src_->read_at(off, raw.data(), raw.size() * 2); !r)
        return r;
    if (t.dtype == DType::F16)
        for (std::size_t i = 0; i < raw.size(); ++i) out[i] = half_to_float(raw[i]);
    else
        for (std::size_t i = 0; i < raw.size(); ++i) out[i] = bf16_to_float(raw[i]);
    return {};
}

}  // namespace hcs::merge::detail
