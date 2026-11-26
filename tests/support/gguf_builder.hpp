#pragma once
// Test helper: builds tiny GGUF v3 files (one string KV + tensors) in memory. Synthetic fixtures only.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hcs::test {

struct GgufBuildTensor {
    std::string name;
    std::uint32_t type = 0;               // ggml type id: 0 = F32, 1 = F16, 8 = Q8_0, 30 = BF16
    std::vector<std::uint64_t> dims;      // GGUF order (ne[0] first = fastest varying)
    std::vector<std::uint8_t> data;
};

inline void put_u32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
inline void put_u64(std::string& s, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
inline void put_str(std::string& s, const std::string& v) {
    put_u64(s, v.size());
    s += v;
}

inline std::string build_gguf(const std::vector<GgufBuildTensor>& tensors, std::uint64_t alignment = 32) {
    std::string h = "GGUF";
    put_u32(h, 3);
    put_u64(h, tensors.size());
    put_u64(h, 1);
    put_str(h, "general.architecture");
    put_u32(h, 8);
    put_str(h, "test");
    std::uint64_t off = 0;
    for (const auto& t : tensors) {
        put_str(h, t.name);
        put_u32(h, static_cast<std::uint32_t>(t.dims.size()));
        for (auto d : t.dims) put_u64(h, d);
        put_u32(h, t.type);
        put_u64(h, off);
        off += (t.data.size() + alignment - 1) / alignment * alignment;
    }
    while (h.size() % alignment != 0) h.push_back('\0');
    std::string out = h;
    for (const auto& t : tensors) {
        out.append(reinterpret_cast<const char*>(t.data.data()), t.data.size());
        while (out.size() % alignment != 0) out.push_back('\0');
    }
    return out;
}

}  // namespace hcs::test
