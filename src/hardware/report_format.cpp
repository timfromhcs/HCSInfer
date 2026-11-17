#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

#include "hw.hpp"

namespace hcs::hardware {

std::string format_bytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    char b[64];
    if (u == 0)
        std::snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(bytes));
    else
        std::snprintf(b, sizeof(b), "%.2f %s", v, units[u]);
    return b;
}

std::string format_mib_s(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.1f MiB/s", v);
    return b;
}

std::string bus_type_name(std::uint32_t t) {
    switch (t) {
        case 0x00:
            return "unknown";
        case 0x01:
            return "scsi";
        case 0x02:
            return "atapi";
        case 0x03:
            return "ata";
        case 0x04:
            return "ieee1394";
        case 0x05:
            return "ssa";
        case 0x06:
            return "fibre";
        case 0x07:
            return "usb";
        case 0x08:
            return "raid";
        case 0x09:
            return "iscsi";
        case 0x0A:
            return "sas";
        case 0x0B:
            return "sata";
        case 0x0C:
            return "sd";
        case 0x0D:
            return "mmc";
        case 0x0E:
            return "virtual";
        case 0x0F:
            return "file_backed_virtual";
        case 0x10:
            return "storage_spaces";
        case 0x11:
            return "nvme";
        case 0x12:
            return "scm";
        case 0x13:
            return "ufs";
        default:
            return "unknown";
    }
}

std::string drive_type_name(std::uint32_t t) {
    switch (t) {
        case 1:
            return "no_root_dir";
        case 2:
            return "removable";
        case 3:
            return "fixed";
        case 4:
            return "remote";
        case 5:
            return "cdrom";
        case 6:
            return "ramdisk";
        default:
            return "unknown";
    }
}

std::string utc_timestamp_now() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

namespace {

std::string str(const json& v) {
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "yes" : "no";
    if (v.is_number_unsigned())
        return std::to_string(v.get<std::uint64_t>());
    if (v.is_number_integer())
        return std::to_string(v.get<std::int64_t>());
    if (v.is_number_float()) {
        char b[48];
        std::snprintf(b, sizeof(b), "%.3f", v.get<double>());
        return b;
    }
    return "unknown";
}

json at(const json& j, std::initializer_list<const char*> path) {
    const json* cur = &j;
    for (const char* k : path) {
        if (!cur->is_object() || !cur->contains(k))
            return unknown();
        cur = &(*cur)[k];
    }
    return *cur;
}

std::string bytes_or_unknown(const json& v) {
    if (v.is_number_unsigned())
        return format_bytes(v.get<std::uint64_t>());
    if (v.is_number_integer() && v.get<std::int64_t>() >= 0)
        return format_bytes(static_cast<std::uint64_t>(v.get<std::int64_t>()));
    return "unknown";
}

std::string join(const json& arr) {
    if (!arr.is_array() || arr.empty())
        return "none";
    std::string s;
    for (const auto& e : arr) {
        if (!s.empty())
            s += ", ";
        s += str(e);
    }
    return s;
}

void line(std::ostringstream& o, const std::string& key, const std::string& value, int indent = 2) {
    o << std::string(static_cast<size_t>(indent), ' ') << key;
    if (key.size() < 26)
        o << std::string(26 - key.size(), ' ');
    o << ' ' << value << "\n";
}

}  // namespace

std::string render_text(const json& r) {
    std::ostringstream o;
    o << "HCSInfer doctor  (schema " << str(at(r, {"schema_version"})) << ", hcsinfer "
      << str(at(r, {"hcsinfer_version"})) << ", " << str(at(r, {"timestamp_utc"})) << ")\n";
    o << "Values reported as 'unknown' could not be detected; nothing is guessed.\n\n";

    o << "System\n";
    line(o, "os:",
         str(at(r, {"os", "name"})) + " " + str(at(r, {"os", "version"})) + " build " +
             str(at(r, {"summary", "os_build"})) + " (" + str(at(r, {"os", "display_version"})) + ")");
    line(o, "cpu:", str(at(r, {"cpu", "name"})));
    line(o, "cpu vendor/family/model:",
         str(at(r, {"cpu", "vendor"})) + " / " + str(at(r, {"cpu", "family"})) + " / " +
             str(at(r, {"cpu", "model"})) + " (stepping " + str(at(r, {"cpu", "stepping"})) + ")");
    line(o, "cores / threads:",
         str(at(r, {"cpu", "physical_cores"})) + " / " + str(at(r, {"cpu", "logical_processors"})));
    line(o, "nominal clock:", str(at(r, {"cpu", "nominal_mhz"})) + " MHz");
    json on = json::array();
    const json feats = at(r, {"cpu", "features"});
    if (feats.is_object())
        for (auto it = feats.begin(); it != feats.end(); ++it)
            if (it.value().is_boolean() && it.value().get<bool>() && it.key().rfind("os_", 0) != 0)
                on.push_back(it.key());
    line(o, "cpu features (usable):", join(on));
    line(o, "ram total:", bytes_or_unknown(at(r, {"memory", "total_physical_bytes"})));
    line(o, "ram installed:", bytes_or_unknown(at(r, {"memory", "installed_physical_bytes"})));
    line(o, "ram available:", bytes_or_unknown(at(r, {"memory", "available_physical_bytes"})));
    line(o, "commit limit:", bytes_or_unknown(at(r, {"memory", "commit_limit_bytes"})));
    line(o, "channels / BIOS UMA:",
         str(at(r, {"memory", "channel_configuration"})) + " / " +
             str(at(r, {"memory", "bios_uma_reservation"})));

    o << "\nGraphics (DXGI / driver registry)\n";
    const json ads = at(r, {"gpu", "adapters"});
    if (ads.is_array() && !ads.empty()) {
        for (const auto& a : ads) {
            line(o, "adapter " + str(at(a, {"index"})) + ":",
                 str(at(a, {"name"})) + " [" + str(at(a, {"vendor_id"})) + ":" + str(at(a, {"device_id"})) +
                     "]" + (at(a, {"software_adapter"}) == json(true) ? " (software)" : ""));
            line(o, "dedicated / shared mem:",
                 bytes_or_unknown(at(a, {"dedicated_video_memory_bytes"})) + " / " +
                     bytes_or_unknown(at(a, {"shared_system_memory_bytes"})),
                 4);
            line(o, "umd version:", str(at(a, {"umd_version"})), 4);
            line(o, "local budget / usage:",
                 bytes_or_unknown(at(a, {"memory_local_segment", "budget_bytes"})) + " / " +
                     bytes_or_unknown(at(a, {"memory_local_segment", "current_usage_bytes"})),
                 4);
            line(o,
                 "non-local budget:", bytes_or_unknown(at(a, {"memory_non_local_segment", "budget_bytes"})),
                 4);
        }
    } else {
        line(o, "adapters:", "unknown (DXGI unavailable)");
    }
    const json regs = at(r, {"gpu", "display_driver_registry"});
    if (regs.is_array())
        for (const auto& e : regs)
            line(o, "driver:",
                 str(at(e, {"description"})) + " " + str(at(e, {"driver_version"})) + " (" +
                     str(at(e, {"driver_date"})) + ", " + str(at(e, {"provider"})) + ")");

    o << "\nVulkan\n";
    line(o, "status:", str(at(r, {"vulkan", "status"})));
    if (at(r, {"vulkan", "reason"}) != unknown())
        line(o, "reason:", str(at(r, {"vulkan", "reason"})));
    if (at(r, {"vulkan", "instance_version"}) != unknown())
        line(o, "instance version:", str(at(r, {"vulkan", "instance_version"})));
    const json devs = at(r, {"vulkan", "devices"});
    if (devs.is_array()) {
        for (const auto& d : devs) {
            line(o, "device " + str(at(d, {"index"})) + ":",
                 str(at(d, {"name"})) + " [" + str(at(d, {"vendor_id"})) + ":" + str(at(d, {"device_id"})) +
                     ", " + str(at(d, {"device_type"})) + "]");
            line(o, "api version:", str(at(d, {"api_version"})), 4);
            line(o, "driver:",
                 str(at(d, {"driver", "name"})) + " " + str(at(d, {"driver", "info"})) + " (id " +
                     str(at(d, {"driver", "id_name"})) + ")",
                 4);
            line(o, "subgroup size:",
                 str(at(d, {"subgroup", "size"})) + " (min " + str(at(d, {"subgroup", "min_size"})) +
                     ", max " + str(at(d, {"subgroup", "max_size"})) + ")",
                 4);
            line(o, "subgroup ops:", join(at(d, {"subgroup", "supported_operations"})), 4);
            line(o, "fp16 / int8 / int16:",
                 str(at(d, {"features", "shader_float16"})) + " / " +
                     str(at(d, {"features", "shader_int8"})) + " / " +
                     str(at(d, {"features", "shader_int16"})),
                 4);
            line(o, "16bit / 8bit storage:",
                 str(at(d, {"features", "storage_buffer_16bit_access"})) + " / " +
                     str(at(d, {"features", "storage_buffer_8bit_access"})),
                 4);
            line(o, "int dot product:", str(at(d, {"features", "shader_integer_dot_product"})), 4);
            line(o, "cooperative matrix:", str(at(d, {"features", "cooperative_matrix_khr"})), 4);
            line(o, "memory budget ext:", str(at(d, {"memory", "memory_budget_extension"})), 4);
            line(o, "shared mem / invocations:",
                 str(at(d, {"limits", "max_compute_shared_memory_size"})) + " B / " +
                     str(at(d, {"limits", "max_compute_work_group_invocations"})),
                 4);
            line(o, "max alloc size:", bytes_or_unknown(at(d, {"max_memory_allocation_size_bytes"})), 4);
            const json heaps = at(d, {"memory", "heaps"});
            if (heaps.is_array())
                for (const auto& h : heaps)
                    line(o, "heap " + str(at(h, {"index"})) + ":",
                         bytes_or_unknown(at(h, {"size_bytes"})) +
                             (at(h, {"device_local"}) == json(true) ? " device-local" : " host") +
                             ", budget " + bytes_or_unknown(at(h, {"budget_bytes"})) + ", usage " +
                             bytes_or_unknown(at(h, {"usage_bytes"})),
                         4);
            const json qf = at(d, {"queue_families"});
            if (qf.is_array())
                for (const auto& q : qf)
                    line(o, "queue family " + str(at(q, {"index"})) + ":",
                         "x" + str(at(q, {"queue_count"})) + " " + join(at(q, {"flags"})), 4);
        }
    }

    o << "\nStorage\n";
    const json rv = at(r, {"storage", "requested_volume"});
    if (rv.is_object() && !rv.contains("error")) {
        line(o, "path:", str(at(r, {"storage", "requested_path"})));
        line(o, "volume:",
             str(at(rv, {"root"})) + "  " + str(at(rv, {"drive_type"})) + ", " + str(at(rv, {"filesystem"})));
        line(o, "bus type:",
             str(at(rv, {"bus_type"})) + ", rotational: " + str(at(rv, {"rotational"})) +
                 ", removable: " + str(at(rv, {"removable_media"})));
        line(o, "device:", str(at(rv, {"device_vendor"})) + " " + str(at(rv, {"device_product"})));
        line(o, "size / free:",
             bytes_or_unknown(at(rv, {"total_bytes"})) + " / " + bytes_or_unknown(at(rv, {"free_bytes"})));
    } else if (rv.is_object()) {
        line(o, "path:", str(at(r, {"storage", "requested_path"})) + " -> " + str(at(rv, {"error"})));
    } else {
        line(o, "requested volume:", "none (use --storage-root <path> or --read-file <file>)");
    }
    const json vols = at(r, {"storage", "volumes"});
    if (vols.is_array())
        for (const auto& v : vols)
            line(o, str(at(v, {"root"})),
                 str(at(v, {"drive_type"})) + ", " + str(at(v, {"filesystem"})) + ", bus " +
                     str(at(v, {"bus_type"})) + ", rotational " + str(at(v, {"rotational"})) + ", " +
                     bytes_or_unknown(at(v, {"total_bytes"})) + " total");
    const json rb = at(r, {"read_benchmark"});
    if (rb.is_object()) {
        o << "\nRead benchmark (read-only, sequential)\n";
        line(o, "status:", str(at(rb, {"status"})));
        if (at(rb, {"error"}) != unknown())
            line(o, "error:", str(at(rb, {"error"})));
        line(o, "buffer:", bytes_or_unknown(at(rb, {"buffer_bytes"})));
        const json samples = at(rb, {"samples"});
        if (samples.is_array())
            for (size_t i = 0; i < samples.size(); ++i)
                line(o, "sample " + std::to_string(i + 1) + ":",
                     bytes_or_unknown(at(samples[i], {"bytes_read"})) + " in " +
                         str(at(samples[i], {"elapsed_ms"})) + " ms = " +
                         (at(samples[i], {"mib_s"}).is_number()
                              ? format_mib_s(at(samples[i], {"mib_s"}).get<double>())
                              : "unknown"));
        if (at(rb, {"mib_s_median"}).is_number())
            line(o, "median:", format_mib_s(at(rb, {"mib_s_median"}).get<double>()));
        line(o, "cold / warm:", str(at(rb, {"cold_or_warm"})));
    }
    return o.str();
}

}  // namespace hcs::hardware
