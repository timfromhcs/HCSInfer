#include "hcs/hardware/memory_probe.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace hcs::hardware {

namespace {
constexpr std::uint64_t kMiB = 1ull << 20;
constexpr std::uint64_t kGiB = 1ull << 30;

#if defined(__APPLE__)
// total = hw.memsize; available = free + inactive + purgeable pages (what the VM can hand out without
// paging).
bool apple_memory(std::uint64_t& total, std::uint64_t& avail) {
    std::size_t len = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0) != 0 || total == 0)
        return false;
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
        KERN_SUCCESS)
        return false;
    const std::uint64_t page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    avail = (static_cast<std::uint64_t>(vm.free_count) + vm.inactive_count + vm.purgeable_count) * page;
    return true;
}
#endif
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
#elif defined(__APPLE__)
    std::uint64_t total = 0, avail = 0;
    if (!apple_memory(total, avail))
        return std::nullopt;
    return make_measurements(total, avail);
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
            s.process_working_set_bytes = rss_pages * static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
        std::fclose(sm);
    }
#elif defined(__APPLE__)
    std::uint64_t total = 0, avail = 0;
    if (apple_memory(total, avail)) {
        s.total_physical_bytes = total;
        s.available_physical_bytes = std::min(avail, total);
    }
    mach_task_basic_info_data_t ti{};
    mach_msg_type_number_t tcount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&ti), &tcount) ==
        KERN_SUCCESS)
        s.process_working_set_bytes = ti.resident_size;
#endif
    return s;
}

}  // namespace hcs::hardware