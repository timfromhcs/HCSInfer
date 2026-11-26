// Typed capability record: pure logic (no Vulkan, no JSON). Decoding from the probe lives in caps_json.cpp.
#include "hcs/hardware/caps.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace hcs::hardware {

const char* to_string(Cap c) noexcept {
    switch (c) {
        case Cap::Yes:
            return "yes";
        case Cap::No:
            return "no";
        default:
            return "unknown";
    }
}

const char* to_string(Support s) noexcept {
    switch (s) {
        case Support::Native:
            return "native";
        case Support::Emulated:
            return "emulated";
        case Support::StorageOnly:
            return "storage_only";
        default:
            return "unsupported";
    }
}

const char* to_string(DType d) noexcept {
    switch (d) {
        case DType::F32:
            return "f32";
        case DType::F16:
            return "f16";
        case DType::I8:
            return "i8";
    }
    return "unknown";
}

std::optional<DType> dtype_from_string(const std::string& s) {
    if (s == "f32")
        return DType::F32;
    if (s == "f16")
        return DType::F16;
    if (s == "i8")
        return DType::I8;
    return std::nullopt;
}

std::string DeviceCaps::device_key() const {
    char b[64];
    std::snprintf(b, sizeof(b), "vk:%04x:%04x", vendor_id, device_id);
    std::string k = b;
    k += ":drv=" + (driver_version_raw.empty() ? std::string("unknown") : driver_version_raw);
    k += ":api=" + (api_version.empty() ? std::string("unknown") : api_version);
    return k;
}

bool DeviceCaps::has_coopmat_f16_f32_16x16x16_subgroup() const {
    if (!is_yes(cooperative_matrix))
        return false;
    return std::any_of(coopmat_shapes.begin(), coopmat_shapes.end(), [](const CoopMatShape& s) {
        return s.m == 16 && s.n == 16 && s.k == 16 && s.a == "f16" && s.b == "f16" && s.c == "f32" &&
               s.result == "f32" && s.scope == 3;
    });
}

std::optional<std::uint32_t> DeviceCaps::compute_queue_family() const {
    for (const auto& q : queues)
        if (q.compute && q.queue_count > 0)
            return q.index;
    return std::nullopt;
}

Support DeviceCaps::dtype_storage(DType d) const {
    if (!compute_queue_family())
        return Support::Unsupported;
    switch (d) {
        case DType::F32:
            return Support::Native;
        case DType::F16:  // packed halves in uint words work without any extension
            return is_yes(storage_16bit) ? Support::Native : Support::Emulated;
        case DType::I8:  // packed int8x4 in int32 words
            return is_yes(storage_8bit) ? Support::Native : Support::Emulated;
    }
    return Support::Unsupported;
}

Support DeviceCaps::dtype_compute(DType d) const {
    if (!compute_queue_family())
        return Support::Unsupported;
    switch (d) {
        case DType::F32:
            return Support::Native;
        case DType::F16:  // storage format; the math is f32 (unpackHalf2x16 is core GLSL)
            return is_yes(shader_float16) ? Support::Native : Support::Emulated;
        case DType::I8:  // dotPacked4x8EXT needs the shaderIntegerDotProduct feature
            if (!is_yes(integer_dot_product))
                return Support::Unsupported;
            return is_yes(int8_dot_accelerated) ? Support::Native : Support::Emulated;
    }
    return Support::Unsupported;
}

namespace {
std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string squash(const std::string& s) {  // lowercase, drop '-', ' ', '_'
    std::string r;
    for (char c : s)
        if (c != '-' && c != ' ' && c != '_')
            r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return r;
}
bool parse_u32(const std::string& s, std::uint32_t& out) {
    if (s.empty())
        return false;
    char* end = nullptr;
    const int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    const unsigned long v = std::strtoul(s.c_str(), &end, base);
    if (end == s.c_str() || *end != '\0' || v > 0xFFFFFFFFul)
        return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}
}  // namespace

std::optional<std::size_t> select_device(const std::vector<DeviceCaps>& devices, const std::string& selector) {
    if (devices.empty())
        return std::nullopt;
    if (selector.empty())
        return std::size_t{0};
    // Plain index.
    {
        std::uint32_t idx = 0;
        const bool all_digits = std::all_of(selector.begin(), selector.end(),
                                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        if (all_digits && parse_u32(selector, idx))
            return idx < devices.size() ? std::optional<std::size_t>(idx) : std::nullopt;
    }
    // Alias / "vendor:device".
    std::uint32_t vendor = 0, device = 0;
    bool have_ids = false;
    const std::string sq = squash(selector);
    if (sq == "radeon680m" || sq == "amdradeon680m") {
        vendor = 0x1002;
        device = 0x1681;
        have_ids = true;
    } else if (const auto colon = selector.find(':'); colon != std::string::npos) {
        have_ids = parse_u32(selector.substr(0, colon), vendor) && parse_u32(selector.substr(colon + 1), device);
    }
    if (have_ids) {
        for (std::size_t i = 0; i < devices.size(); ++i)
            if (devices[i].vendor_id == vendor && devices[i].device_id == device)
                return i;
        return std::nullopt;
    }
    // Name substring.
    const std::string needle = lower(selector);
    for (std::size_t i = 0; i < devices.size(); ++i)
        if (lower(devices[i].name).find(needle) != std::string::npos)
            return i;
    return std::nullopt;
}

}  // namespace hcs::hardware
