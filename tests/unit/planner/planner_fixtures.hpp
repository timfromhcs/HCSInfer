#pragma once
// Synthetic probe records in the exact shape src/hardware/vulkan_info.cpp emits. The "radeon_680m" record
// carries the values hcsuda measured on the owner's machine (benchmarks/amd-radeon-680m/caps.json,
// docs/MEASURED.md); "old_vulkan_device" is a Vulkan 1.1 device that reports almost nothing ("unknown").
#include <nlohmann/json.hpp>

#include "caps_json.hpp"
#include "hcs/hardware/kernel_profile.hpp"
#include "hcs/hardware/planner.hpp"

namespace hcs::test {

using nlohmann::json;
using namespace hcs::hardware;

inline json radeon_680m_probe() {
    json d;
    d["index"] = 0;
    d["name"] = "AMD Radeon(TM) Graphics";
    d["device_type"] = "integrated_gpu";
    d["vendor_id"] = "0x1002";
    d["device_id"] = "0x1681";
    d["api_version"] = "1.4.315";
    d["driver_version_raw"] = "0x8008A000";
    d["driver"] = {{"id_name", "amd_proprietary"}, {"name", "AMD proprietary driver"},
                   {"info", "26.6.1 (AMD proprietary shader compiler)"}};
    d["subgroup"] = {{"size", 64}, {"supported_operations", {"basic", "vote", "arithmetic", "ballot"}}};
    d["integer_dot_product"] = {{"int8_signed_accelerated", true},
                                {"int4x8_packed_signed_accelerated", true},
                                {"int16_signed_accelerated", true}};
    d["limits"] = {{"max_compute_shared_memory_size", 32768},  {"max_compute_work_group_invocations", 1024},
                   {"max_storage_buffer_range", 4294967295ull}, {"max_push_constants_size", 256},
                   {"timestamp_period_ns", 10.0},               {"timestamp_compute_and_graphics", true}};
    d["features"] = {{"shader_float16", true},
                     {"storage_buffer_16bit_access", true},
                     {"shader_int8", true},
                     {"storage_buffer_8bit_access", true},
                     {"shader_integer_dot_product", true},
                     {"cooperative_matrix_khr", true}};
    d["relevant_extensions"] = {{"VK_KHR_cooperative_matrix", true},
                                {"VK_EXT_shader_float8", false},
                                {"VK_KHR_shader_bfloat16", false},
                                {"VK_EXT_memory_budget", true}};
    json shapes = json::array();
    shapes.push_back({{"m", 16}, {"n", 16}, {"k", 16}, {"a", "f16"}, {"b", "f16"}, {"c", "f32"},
                      {"result", "f32"}, {"scope", 3}});
    shapes.push_back({{"m", 16}, {"n", 16}, {"k", 16}, {"a", "s8"}, {"b", "s8"}, {"c", "s32"},
                      {"result", "s32"}, {"scope", 3}});
    d["cooperative_matrix_properties"] = shapes;
    d["memory"] = {{"heaps",
                    {{{"index", 0}, {"size_bytes", 9766305792ull}, {"device_local", true}, {"budget_bytes", 8000000000ull}},
                     {{"index", 1}, {"size_bytes", 4883152896ull}, {"device_local", false}, {"budget_bytes", "unknown"}}}}};
    d["queue_families"] = {{{"index", 0}, {"queue_count", 1}, {"flags", {"graphics", "compute", "transfer"}},
                            {"timestamp_valid_bits", 64}}};
    return d;
}

inline json old_vulkan_device_probe() {
    json d;
    d["index"] = 1;
    d["name"] = "Old Vulkan 1.1 GPU";
    d["device_type"] = "discrete_gpu";
    d["vendor_id"] = "0x10DE";
    d["device_id"] = "0x1234";
    d["api_version"] = "1.1.0";
    d["driver_version_raw"] = "0x00000001";
    d["driver"] = "unknown";
    d["subgroup"] = "unknown";
    d["integer_dot_product"] = "unknown";
    d["limits"] = "unknown";
    d["features"] = "unknown";
    d["relevant_extensions"] = {{"VK_KHR_cooperative_matrix", false}};
    d["cooperative_matrix_properties"] = json::array();
    d["memory"] = "unknown";
    d["queue_families"] = {{{"index", 0}, {"queue_count", 1}, {"flags", {"graphics", "compute"}},
                            {"timestamp_valid_bits", 0}}};
    return d;
}

inline DeviceCaps caps_680m() {
    return caps_from_probe(radeon_680m_probe());
}

// A profile entry that passes every promotion check, for the 680M, with the given kernel/bucket/hash.
inline ProfileEntry good_entry(const DeviceCaps& caps, const std::string& kernel, const std::string& hash,
                               const Shape& shape) {
    ProfileEntry e;
    e.device_key = caps.device_key();
    e.kernel_id = kernel;
    e.kernel_hash = hash;
    e.shape_bucket = shape_bucket(shape);
    e.device_name = caps.name;
    e.driver_info = caps.driver_info;
    e.m = shape.m;
    e.n = shape.n;
    e.k = shape.k;
    e.timestamp_utc = "2026-11-26T00:00:00Z";
    e.correct = true;
    e.max_abs_error = 1e-6;
    e.tol_rel = 1e-5;
    e.tol_abs = 3e-5;
    e.identical_runs = 3;
    e.total_runs = 3;
    e.samples = 15;
    e.wall_ms_median = 1.0;
    e.wall_spread = 0.05;
    e.gpu_ms_median = 0.4;
    e.gpu_spread = 0.02;
    e.flops = 1e8;
    e.baseline_name = "cpu_mt_autovec";
    e.baseline_ms = 2.0;  // 2x
    e.upstream_status = "not_run";
    return e;
}

}  // namespace hcs::test
