#pragma once
// Typed Vulkan device capability record (AGENTS.md 12.2). Decoded from the doctor's Vulkan probe, never
// inferred from a device name. Anything the probe did not positively report is `Unknown` / absent and is
// treated as "not available" by the planner (hcsuda's "unknown = CPU" rule).
//
// Ported from hcsuda @94a3485 crates/hcsuda-core/src/caps.rs (model) and crates/hcsuda-vk/src/device.rs
// (feature selection), see docs/hcsuda-port.md.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hcs::hardware {

// Tri-state for a probed capability: Unknown is NOT the same as No.
enum class Cap : std::uint8_t { Unknown = 0, No = 1, Yes = 2 };
const char* to_string(Cap c) noexcept;
constexpr bool is_yes(Cap c) noexcept {
    return c == Cap::Yes;
}

// How well the device can handle a number format with the kernels HCSInfer actually owns.
enum class Support : std::uint8_t { Unsupported = 0, StorageOnly = 1, Emulated = 2, Native = 3 };
const char* to_string(Support s) noexcept;
constexpr bool can_compute(Support s) noexcept {
    return s == Support::Native || s == Support::Emulated;
}

enum class DType : std::uint8_t { F32 = 0, F16 = 1, I8 = 2 };
const char* to_string(DType d) noexcept;
std::optional<DType> dtype_from_string(const std::string& s);

struct HeapCaps {
    std::uint32_t index = 0;
    std::uint64_t size_bytes = 0;
    std::optional<std::uint64_t> budget_bytes;  // only with VK_EXT_memory_budget
    bool device_local = false;
};

struct CoopMatShape {
    std::uint32_t m = 0, n = 0, k = 0;
    std::string a, b, c, result;  // component types: "f16", "f32", "s8", "s32", ...
    std::uint32_t scope = 0;      // VkScopeKHR (3 = subgroup)
};

struct QueueFamilyCaps {
    std::uint32_t index = 0;
    std::uint32_t queue_count = 0;
    bool compute = false, graphics = false, transfer = false;
    std::uint32_t timestamp_valid_bits = 0;
};

struct DeviceCaps {
    // Identity.
    std::uint32_t probe_index = 0;  // index in the Vulkan enumeration the probe used
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::string device_type;  // integrated_gpu | discrete_gpu | ...
    std::string api_version;  // "1.4.315"
    std::string driver_version_raw;  // "0x..." as the probe printed it
    std::string driver_name, driver_info;  // empty = unknown (Vulkan < 1.2)

    // Subgroup / limits. nullopt = unknown.
    std::optional<std::uint32_t> subgroup_size;
    std::vector<std::string> subgroup_ops;
    std::optional<std::uint32_t> max_workgroup_invocations;
    std::optional<std::uint32_t> max_shared_memory;
    std::optional<std::uint64_t> max_storage_range;
    std::optional<std::uint32_t> max_push_constants;
    std::optional<double> timestamp_period_ns;

    // Features (tri-state).
    Cap shader_float16 = Cap::Unknown;
    Cap storage_16bit = Cap::Unknown;
    Cap shader_int8 = Cap::Unknown;
    Cap storage_8bit = Cap::Unknown;
    Cap integer_dot_product = Cap::Unknown;   // shaderIntegerDotProduct feature
    Cap int8_dot_accelerated = Cap::Unknown;  // 4x8-bit packed signed dot is hardware accelerated
    Cap cooperative_matrix = Cap::Unknown;    // VK_KHR_cooperative_matrix feature
    Cap float8_ext = Cap::Unknown;            // VK_EXT_shader_float8 extension present
    Cap bfloat16_ext = Cap::Unknown;          // VK_KHR_shader_bfloat16 extension present
    Cap memory_budget_ext = Cap::Unknown;
    Cap timestamps = Cap::Unknown;  // compute queue can write timestamps

    std::vector<CoopMatShape> coopmat_shapes;
    std::vector<HeapCaps> heaps;
    std::vector<QueueFamilyCaps> queues;

    // Stable cache key: vendor:device:driver:api. A profile entry is only valid for the exact key.
    std::string device_key() const;
    // True when the device exposes f16 x f16 -> f32 at subgroup scope with a 16x16x16 shape.
    bool has_coopmat_f16_f32_16x16x16_subgroup() const;
    // Number format support for the kernels HCSInfer owns (conservative; unknown => Unsupported).
    Support dtype_compute(DType d) const;
    Support dtype_storage(DType d) const;
    // Index of the first compute-capable queue family, if any.
    std::optional<std::uint32_t> compute_queue_family() const;
};

// Looks up a device by selector: "<index>", an alias ("radeon-680m" -> 1002:1681), "0x1002:0x1681", or a
// case-insensitive substring of the device name. Returns the index into `devices` or nullopt.
std::optional<std::size_t> select_device(const std::vector<DeviceCaps>& devices, const std::string& selector);

// Runs the Vulkan probe (vulkan-1.dll is loaded dynamically; nothing links Vulkan) and decodes every physical
// device. Empty when Vulkan is unavailable; `status` (optional) receives the probe status string
// (ok|unavailable|failed|no_devices|skipped) and `reason` the human-readable cause when not ok.
std::vector<DeviceCaps> probe_vulkan_caps(std::string* status = nullptr, std::string* reason = nullptr);

}  // namespace hcs::hardware