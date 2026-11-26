#pragma once
// Test helper: builds tiny safetensors files in memory (header JSON + raw data). Synthetic fixtures only.
#include <cstdint>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace hcs::test {

struct StBuildTensor {
    std::string name;
    std::string dtype = "F32";
    std::vector<std::uint64_t> shape;
    std::vector<std::uint8_t> data;
};

inline std::vector<std::uint8_t> f32_bytes(const std::vector<float>& v) {
    std::vector<std::uint8_t> b(v.size() * 4);
    if (!v.empty())
        std::memcpy(b.data(), v.data(), b.size());
    return b;
}

inline StBuildTensor f32_tensor(const std::string& name, std::vector<std::uint64_t> shape,
                                const std::vector<float>& values) {
    StBuildTensor t;
    t.name = name;
    t.shape = std::move(shape);
    t.data = f32_bytes(values);
    return t;
}

inline std::string build_safetensors(const std::vector<StBuildTensor>& tensors,
                                     const nlohmann::json& metadata = nlohmann::json()) {
    nlohmann::json h = nlohmann::json::object();
    if (!metadata.is_null())
        h["__metadata__"] = metadata;
    std::uint64_t off = 0;
    for (const auto& t : tensors) {
        h[t.name] = {{"dtype", t.dtype}, {"shape", t.shape}, {"data_offsets", {off, off + t.data.size()}}};
        off += t.data.size();
    }
    std::string hs = h.dump();
    std::string out;
    const std::uint64_t n = hs.size();
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
    out += hs;
    for (const auto& t : tensors) out.append(reinterpret_cast<const char*>(t.data.data()), t.data.size());
    return out;
}

}  // namespace hcs::test
