// Probe JSON -> DeviceCaps and back. Pure (no Vulkan); unit-testable with synthetic probe records.
#include "caps_json.hpp"

#include <cstdlib>

namespace hcs::hardware {

namespace {

Cap cap_of(const json& obj, const char* key) {
    if (!obj.is_object())
        return Cap::Unknown;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean())
        return Cap::Unknown;  // missing, or the probe's "unknown" string
    return it->get<bool>() ? Cap::Yes : Cap::No;
}

std::optional<std::uint32_t> u32_of(const json& obj, const char* key) {
    if (!obj.is_object())
        return std::nullopt;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer() || it->get<std::int64_t>() < 0 ||
        it->get<std::int64_t>() > 0xFFFFFFFFll)
        return std::nullopt;
    return static_cast<std::uint32_t>(it->get<std::int64_t>());
}

std::uint32_t hex_id(const json& v) {
    if (!v.is_string())
        return 0;
    return static_cast<std::uint32_t>(std::strtoul(v.get<std::string>().c_str(), nullptr, 16));
}

std::string str_of(const json& obj, const char* key) {
    if (!obj.is_object())
        return {};
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool has_flag(const json& arr, const char* name) {
    if (!arr.is_array())
        return false;
    for (const auto& e : arr)
        if (e.is_string() && e.get<std::string>() == name)
            return true;
    return false;
}

}  // namespace

DeviceCaps caps_from_probe(const json& d) {
    DeviceCaps c;
    c.probe_index = u32_of(d, "index").value_or(0);
    c.name = str_of(d, "name");
    c.vendor_id = d.contains("vendor_id") ? hex_id(d["vendor_id"]) : 0;
    c.device_id = d.contains("device_id") ? hex_id(d["device_id"]) : 0;
    c.device_type = str_of(d, "device_type");
    c.api_version = str_of(d, "api_version");
    c.driver_version_raw = str_of(d, "driver_version_raw");

    if (d.contains("driver") && d["driver"].is_object()) {
        c.driver_name = str_of(d["driver"], "name");
        c.driver_info = str_of(d["driver"], "info");
    }
    if (d.contains("subgroup") && d["subgroup"].is_object()) {
        c.subgroup_size = u32_of(d["subgroup"], "size");
        if (d["subgroup"].contains("supported_operations") && d["subgroup"]["supported_operations"].is_array())
            for (const auto& e : d["subgroup"]["supported_operations"])
                if (e.is_string())
                    c.subgroup_ops.push_back(e.get<std::string>());
    }
    if (d.contains("limits") && d["limits"].is_object()) {
        const json& L = d["limits"];
        c.max_workgroup_invocations = u32_of(L, "max_compute_work_group_invocations");
        c.max_shared_memory = u32_of(L, "max_compute_shared_memory_size");
        c.max_push_constants = u32_of(L, "max_push_constants_size");
        if (L.contains("max_storage_buffer_range") && L["max_storage_buffer_range"].is_number_integer() &&
            L["max_storage_buffer_range"].get<std::int64_t>() >= 0)
            c.max_storage_range = L["max_storage_buffer_range"].get<std::uint64_t>();
        if (L.contains("timestamp_period_ns") && L["timestamp_period_ns"].is_number())
            c.timestamp_period_ns = L["timestamp_period_ns"].get<double>();
    }
    const json empty = json::object();
    const json& F = (d.contains("features") && d["features"].is_object()) ? d["features"] : empty;
    c.shader_float16 = cap_of(F, "shader_float16");
    c.storage_16bit = cap_of(F, "storage_buffer_16bit_access");
    c.shader_int8 = cap_of(F, "shader_int8");
    c.storage_8bit = cap_of(F, "storage_buffer_8bit_access");
    c.integer_dot_product = cap_of(F, "shader_integer_dot_product");
    c.cooperative_matrix = cap_of(F, "cooperative_matrix_khr");

    if (d.contains("integer_dot_product") && d["integer_dot_product"].is_object())
        c.int8_dot_accelerated = cap_of(d["integer_dot_product"], "int4x8_packed_signed_accelerated");

    if (d.contains("relevant_extensions") && d["relevant_extensions"].is_object()) {
        const json& R = d["relevant_extensions"];
        c.float8_ext = cap_of(R, "VK_EXT_shader_float8");
        c.bfloat16_ext = cap_of(R, "VK_KHR_shader_bfloat16");
        c.memory_budget_ext = cap_of(R, "VK_EXT_memory_budget");
        // The coopmat *feature* is only trusted when the extension is there too.
        if (cap_of(R, "VK_KHR_cooperative_matrix") == Cap::No)
            c.cooperative_matrix = Cap::No;
    }

    if (d.contains("cooperative_matrix_properties") && d["cooperative_matrix_properties"].is_array()) {
        for (const auto& e : d["cooperative_matrix_properties"]) {
            CoopMatShape s;
            s.m = u32_of(e, "m").value_or(0);
            s.n = u32_of(e, "n").value_or(0);
            s.k = u32_of(e, "k").value_or(0);
            s.a = str_of(e, "a");
            s.b = str_of(e, "b");
            s.c = str_of(e, "c");
            s.result = str_of(e, "result");
            s.scope = u32_of(e, "scope").value_or(0);
            c.coopmat_shapes.push_back(std::move(s));
        }
    }

    if (d.contains("memory") && d["memory"].is_object() && d["memory"].contains("heaps") &&
        d["memory"]["heaps"].is_array()) {
        for (const auto& h : d["memory"]["heaps"]) {
            HeapCaps hc;
            hc.index = u32_of(h, "index").value_or(0);
            if (h.contains("size_bytes") && h["size_bytes"].is_number_integer() && h["size_bytes"].get<std::int64_t>() >= 0)
                hc.size_bytes = h["size_bytes"].get<std::uint64_t>();
            hc.device_local = h.contains("device_local") && h["device_local"].is_boolean() &&
                              h["device_local"].get<bool>();
            if (h.contains("budget_bytes") && h["budget_bytes"].is_number_integer() && h["budget_bytes"].get<std::int64_t>() >= 0)
                hc.budget_bytes = h["budget_bytes"].get<std::uint64_t>();
            c.heaps.push_back(hc);
        }
    }

    bool any_timestamp_queue = false;
    if (d.contains("queue_families") && d["queue_families"].is_array()) {
        for (const auto& q : d["queue_families"]) {
            QueueFamilyCaps qc;
            qc.index = u32_of(q, "index").value_or(0);
            qc.queue_count = u32_of(q, "queue_count").value_or(0);
            qc.compute = q.contains("flags") && has_flag(q["flags"], "compute");
            qc.graphics = q.contains("flags") && has_flag(q["flags"], "graphics");
            qc.transfer = q.contains("flags") && has_flag(q["flags"], "transfer");
            qc.timestamp_valid_bits = u32_of(q, "timestamp_valid_bits").value_or(0);
            if (qc.compute && qc.timestamp_valid_bits > 0)
                any_timestamp_queue = true;
            c.queues.push_back(qc);
        }
        // Compute timestamps are usable when a compute-capable queue family reports valid timestamp bits.
        c.timestamps = any_timestamp_queue ? Cap::Yes : Cap::No;
    }
    return c;
}

std::vector<DeviceCaps> caps_from_report(const json& report) {
    std::vector<DeviceCaps> out;
    if (!report.is_object() || !report.contains("devices") || !report["devices"].is_array())
        return out;
    for (const auto& d : report["devices"])
        out.push_back(caps_from_probe(d));
    return out;
}

json caps_to_json(const DeviceCaps& c) {
    auto opt_u32 = [](const std::optional<std::uint32_t>& v) { return v ? json(*v) : unknown(); };
    json j;
    j["name"] = c.name;
    j["vendor_id"] = c.vendor_id;
    j["device_id"] = c.device_id;
    j["device_key"] = c.device_key();
    j["subgroup_size"] = opt_u32(c.subgroup_size);
    j["subgroup_ops"] = c.subgroup_ops;
    j["max_workgroup_invocations"] = opt_u32(c.max_workgroup_invocations);
    j["max_shared_memory"] = opt_u32(c.max_shared_memory);
    j["max_push_constants"] = opt_u32(c.max_push_constants);
    j["max_storage_range"] = c.max_storage_range ? json(*c.max_storage_range) : unknown();
    j["timestamp_period_ns"] = c.timestamp_period_ns ? json(*c.timestamp_period_ns) : unknown();
    j["features"] = {{"shader_float16", to_string(c.shader_float16)},
                     {"storage_16bit", to_string(c.storage_16bit)},
                     {"shader_int8", to_string(c.shader_int8)},
                     {"storage_8bit", to_string(c.storage_8bit)},
                     {"integer_dot_product", to_string(c.integer_dot_product)},
                     {"int8_dot_accelerated", to_string(c.int8_dot_accelerated)},
                     {"cooperative_matrix", to_string(c.cooperative_matrix)},
                     {"float8_ext", to_string(c.float8_ext)},
                     {"bfloat16_ext", to_string(c.bfloat16_ext)},
                     {"memory_budget_ext", to_string(c.memory_budget_ext)},
                     {"timestamps", to_string(c.timestamps)}};
    j["coopmat_f16_f32_16x16x16_subgroup"] = c.has_coopmat_f16_f32_16x16x16_subgroup();
    json dt = json::object();
    for (DType d : {DType::F32, DType::F16, DType::I8})
        dt[to_string(d)] = {{"storage", to_string(c.dtype_storage(d))}, {"compute", to_string(c.dtype_compute(d))}};
    j["dtypes"] = dt;
    const auto cq = c.compute_queue_family();
    j["compute_queue_family"] = cq ? json(*cq) : unknown();
    return j;
}

std::vector<DeviceCaps> probe_vulkan_caps(std::string* status, std::string* reason) {
    const json r = vulkan_report(false);
    if (status)
        *status = r.value("status", std::string("unknown"));
    if (reason)
        *reason = r.value("reason", std::string());
    return caps_from_report(r);
}

}  // namespace hcs::hardware