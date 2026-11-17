#pragma once
// Internal declarations of the hardware module (not part of the public API).
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace hcs::hardware {

using json = nlohmann::json;

inline json unknown() {
    return "unknown";
}

// --- cpu_info.cpp -------------------------------------------------------------------------
struct CpuidRegs {
    std::uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
};
struct CpuRaw {
    std::string vendor;
    std::string brand;
    std::uint32_t max_leaf = 0;
    std::uint32_t max_ext_leaf = 0;
    CpuidRegs l1, l7, l7_1;
    CpuidRegs l80000001;
    bool has_xgetbv = false;  // OSXSAVE set
    std::uint64_t xcr0 = 0;
};
// Pure decoding (unit-testable with synthetic data).
json decode_cpu_features(const CpuRaw& raw);
json decode_cpu_identity(const CpuRaw& raw);
CpuRaw read_cpu_raw();
json cpu_report();

// --- system_info.cpp ----------------------------------------------------------------------
json system_report();  // {"os":{...}, "memory":{...}}

// --- gpu_info.cpp -------------------------------------------------------------------------
json gpu_report();  // DXGI adapters + display-class driver registry entries

// --- vulkan_info.cpp ----------------------------------------------------------------------
json vulkan_report(bool skip);

// --- storage_info.cpp ---------------------------------------------------------------------
struct ReadBenchOptions {
    std::uint64_t max_bytes = 256ull << 20;
    std::uint32_t buffer_bytes = 4u << 20;
    std::uint32_t repeats = 1;
};
json storage_report(const std::string& storage_root, bool list_volumes);
json read_benchmark(const std::string& file, const ReadBenchOptions& opt);

// --- report_format.cpp --------------------------------------------------------------------
std::string format_bytes(std::uint64_t bytes);
std::string format_mib_s(double v);
std::string bus_type_name(std::uint32_t bus_type);
std::string drive_type_name(std::uint32_t drive_type);
std::string utc_timestamp_now();
std::string render_text(const json& report);

}  // namespace hcs::hardware
