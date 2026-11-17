#include "hcs/hardware/memory_probe.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hcs::hardware {

namespace {
constexpr std::uint64_t kMiB = 1ull << 20;
constexpr std::uint64_t kGiB = 1ull << 30;
}  // namespace

MemoryMeasurements make_measurements(std::uint64_t total_physical_bytes,
                                     std::uint64_t available_physical_bytes) {
    MemoryMeasurements m;
    m.available_physical_bytes = std::min(available_physical_bytes, total_physical_bytes);
    m.os_reserve_bytes = std::max<std::uint64_t>(kGiB, total_physical_bytes / 10);
    m.safety_margin_bytes = 512 * kMiB;
    return m;
}

std::optional<MemoryMeasurements> measure_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms) || ms.ullTotalPhys == 0)
        return std::nullopt;
    return make_measurements(ms.ullTotalPhys, ms.ullAvailPhys);
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f)
        return std::nullopt;
    unsigned long long total_kb = 0, avail_kb = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::sscanf(line, "MemTotal: %llu kB", &total_kb);
        std::sscanf(line, "MemAvailable: %llu kB", &avail_kb);
    }
    std::fclose(f);
    if (total_kb == 0)
        return std::nullopt;
    return make_measurements(total_kb * 1024ull, avail_kb * 1024ull);
#else
    return std::nullopt;
#endif
}

}  // namespace hcs::hardware
