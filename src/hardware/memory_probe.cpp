#include "hcs/hardware/memory_probe.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
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

SystemMemorySample sample_system_memory() {
    SystemMemorySample s;
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys != 0) {
        s.total_physical_bytes = ms.ullTotalPhys;
        s.available_physical_bytes = std::min<std::uint64_t>(ms.ullAvailPhys, ms.ullTotalPhys);
    }
    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi)) && pi.PageSize != 0 && pi.CommitLimit != 0) {
        s.commit_used_bytes = static_cast<std::uint64_t>(pi.CommitTotal) * pi.PageSize;
        s.commit_limit_bytes = static_cast<std::uint64_t>(pi.CommitLimit) * pi.PageSize;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        s.process_working_set_bytes = static_cast<std::uint64_t>(pmc.WorkingSetSize);
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (f) {
        unsigned long long total_kb = 0, avail_kb = 0, commit_limit_kb = 0, committed_kb = 0;
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            std::sscanf(line, "MemTotal: %llu kB", &total_kb);
            std::sscanf(line, "MemAvailable: %llu kB", &avail_kb);
            std::sscanf(line, "CommitLimit: %llu kB", &commit_limit_kb);
            std::sscanf(line, "Committed_AS: %llu kB", &committed_kb);
        }
        std::fclose(f);
        if (total_kb != 0) {
            s.total_physical_bytes = total_kb * 1024ull;
            s.available_physical_bytes = avail_kb * 1024ull;
        }
        if (commit_limit_kb != 0) {
            s.commit_limit_bytes = commit_limit_kb * 1024ull;
            s.commit_used_bytes = committed_kb * 1024ull;
        }
    }
    std::FILE* sm = std::fopen("/proc/self/statm", "r");
    if (sm) {
        unsigned long long size_pages = 0, rss_pages = 0;
        if (std::fscanf(sm, "%llu %llu", &size_pages, &rss_pages) == 2)
            s.process_working_set_bytes = rss_pages * 4096ull;
        std::fclose(sm);
    }
#endif
    return s;
}

}  // namespace hcs::hardware