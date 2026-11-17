#include "hcs/hardware/doctor.hpp"

#include <algorithm>
#include <ostream>

#include "hcs/result.hpp"
#include "hcs/version.hpp"
#include "hw.hpp"

namespace hcs::hardware {

namespace {

constexpr std::uint32_t kMinBuffer = 64u << 10;
constexpr std::uint32_t kMaxBuffer = 64u << 20;

std::string validate(const DoctorOptions& o) {
    if (o.read_buffer_bytes < kMinBuffer || o.read_buffer_bytes > kMaxBuffer)
        return "read buffer must be between 64 KiB and 64 MiB";
    if (o.read_repeats < 1 || o.read_repeats > 5)
        return "read repeats must be between 1 and 5";
    if (o.read_max_bytes < 1)
        return "read max bytes must be >= 1";
    return {};
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "0x1002" -> "1002"
std::string id_digits(const json& v) {
    if (!v.is_string())
        return {};
    std::string s = v.get<std::string>();
    if (s.rfind("0x", 0) == 0)
        s = s.substr(2);
    return upper(s);
}

json build_summary(const json& r) {
    json s;
    const json& os = r["os"];
    const json& cpu = r["cpu"];
    s["cpu"] = cpu.value("name", unknown());
    json feats = json::array();
    if (cpu["features"].is_object())
        for (auto it = cpu["features"].begin(); it != cpu["features"].end(); ++it)
            if (it.value().is_boolean() && it.value().get<bool>() && it.key().rfind("os_", 0) != 0)
                feats.push_back(it.key());
    s["cpu_features"] = feats;
    s["ram_bytes"] = r["memory"]["total_physical_bytes"];
    if (os["build"].is_number()) {
        std::string b = "10.0." + std::to_string(os["build"].get<std::uint32_t>());
        if (os["ubr"].is_number())
            b += "." + std::to_string(os["ubr"].get<std::uint32_t>());
        s["os_build"] = b;
    } else {
        s["os_build"] = unknown();
    }
    s["gpu"] = unknown();
    s["gpu_vendor_id"] = unknown();
    s["gpu_device_id"] = unknown();
    s["driver_version"] = unknown();
    s["vulkan_api_version"] = unknown();
    s["uma_reservation_bytes"] = unknown();  // DXGI dedicated video memory, not the BIOS setting itself
    s["vulkan_status"] = r["vulkan"]["status"];

    // Prefer the first Vulkan device; fall back to the first non-software DXGI adapter.
    const json& vd = r["vulkan"]["devices"];
    if (vd.is_array() && !vd.empty()) {
        const json& d = vd[0];
        s["gpu"] = d["name"];
        s["gpu_vendor_id"] = d["vendor_id"];
        s["gpu_device_id"] = d["device_id"];
        s["vulkan_api_version"] = d["api_version"];
        if (d.contains("driver") && d["driver"].is_object() && d["driver"].contains("info") &&
            d["driver"]["info"].is_string() && !d["driver"]["info"].get<std::string>().empty())
            s["driver_version"] = d["driver"]["info"];
    } else {
        for (const auto& a : r["gpu"]["adapters"])
            if (a.value("software_adapter", false) == false) {
                s["gpu"] = a["name"];
                s["gpu_vendor_id"] = a["vendor_id"];
                s["gpu_device_id"] = a["device_id"];
                break;
            }
    }
    // DXGI memory + registry driver version for the selected adapter.
    const std::string ven = id_digits(s["gpu_vendor_id"]);
    const std::string dev = id_digits(s["gpu_device_id"]);
    if (!ven.empty() && !dev.empty()) {
        for (const auto& a : r["gpu"]["adapters"])
            if (id_digits(a["vendor_id"]) == ven && id_digits(a["device_id"]) == dev) {
                s["uma_reservation_bytes"] = a["dedicated_video_memory_bytes"];
                break;
            }
        for (const auto& e : r["gpu"]["display_driver_registry"]) {
            const std::string id = e.contains("matching_device_id") && e["matching_device_id"].is_string()
                                       ? upper(e["matching_device_id"].get<std::string>())
                                       : std::string();
            if (id.find("VEN_" + ven) != std::string::npos && id.find("DEV_" + dev) != std::string::npos) {
                s["registry_driver_version"] = e["driver_version"];
                if (!s["driver_version"].is_string() || s["driver_version"] == "unknown")
                    s["driver_version"] = e["driver_version"];
                break;
            }
        }
    }
    return s;
}

json build_report(const DoctorOptions& o) {
    json r;
    r["schema_version"] = 1;
    r["hcsinfer_version"] = HCS_VERSION_STRING;
    r["timestamp_utc"] = utc_timestamp_now();
    r["options"] = {{"no_vulkan", o.no_vulkan}};
    json sys = system_report();
    r["os"] = sys["os"];
    r["memory"] = sys["memory"];
    r["cpu"] = cpu_report();
    r["gpu"] = gpu_report();
    r["vulkan"] = vulkan_report(o.no_vulkan);
    std::string root = o.storage_root.empty() ? o.read_file : o.storage_root;
    r["storage"] = storage_report(root, true);
    if (!o.read_file.empty()) {
        ReadBenchOptions b;
        b.max_bytes = o.read_max_bytes;
        b.buffer_bytes = o.read_buffer_bytes;
        b.repeats = o.read_repeats;
        r["read_benchmark"] = read_benchmark(o.read_file, b);
    }
    r["summary"] = build_summary(r);
    return r;
}

std::string dump(const json& j) {
    return j.dump(2, ' ', false, json::error_handler_t::replace);
}

}  // namespace

std::string collect_report_json(const DoctorOptions& options, bool* ok) {
    const std::string err = validate(options);
    if (!err.empty()) {
        if (ok)
            *ok = false;
        return dump(json{{"error", err}});
    }
    if (ok)
        *ok = true;
    return dump(build_report(options));
}

int run_doctor(const DoctorOptions& options, std::ostream& out, std::ostream& err) {
    const std::string bad = validate(options);
    if (!bad.empty()) {
        err << "error: " << bad << " [invalid_argument]\n";
        return exit_code(ErrorCode::InvalidArgument);
    }
    try {
        const json report = build_report(options);
        if (options.json)
            out << dump(report) << "\n";
        else
            out << render_text(report);
        return 0;
    } catch (const std::exception& e) {
        err << "error: doctor failed: " << e.what() << " [internal]\n";
        return exit_code(ErrorCode::Internal);
    }
}

int run_doctor(bool json_out, std::ostream& out) {
    DoctorOptions o;
    o.json = json_out;
    return run_doctor(o, out, out);
}

}  // namespace hcs::hardware
