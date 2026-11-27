#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "hw.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace hcs::hardware {

namespace {
constexpr bool bit(std::uint32_t v, int n) {
    return ((v >> n) & 1u) != 0;
}

#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
#define HCS_X86 1
CpuidRegs cpuid(std::uint32_t leaf, std::uint32_t sub = 0) {
    CpuidRegs r;
#if defined(_MSC_VER)
    int a[4];
    __cpuidex(a, static_cast<int>(leaf), static_cast<int>(sub));
    r.eax = static_cast<std::uint32_t>(a[0]);
    r.ebx = static_cast<std::uint32_t>(a[1]);
    r.ecx = static_cast<std::uint32_t>(a[2]);
    r.edx = static_cast<std::uint32_t>(a[3]);
#else
    __cpuid_count(leaf, sub, r.eax, r.ebx, r.ecx, r.edx);
#endif
    return r;
}
std::string regs_to_string(std::initializer_list<std::uint32_t> words) {
    std::string s;
    for (std::uint32_t w : words) s.append(reinterpret_cast<const char*>(&w), 4);
    return s;
}
std::string trim(std::string s) {
    s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(' ');
    return s.substr(b, e - b + 1);
}
#endif

#if defined(__linux__)
std::string trim_ws(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}
#endif
}  // namespace

CpuRaw read_cpu_raw() {
    CpuRaw raw;
#ifdef HCS_X86
    CpuidRegs l0 = cpuid(0);
    raw.max_leaf = l0.eax;
    raw.vendor = trim(regs_to_string({l0.ebx, l0.edx, l0.ecx}));
    if (raw.max_leaf >= 1)
        raw.l1 = cpuid(1);
    if (raw.max_leaf >= 7) {
        raw.l7 = cpuid(7, 0);
        if (raw.l7.eax >= 1)
            raw.l7_1 = cpuid(7, 1);
    }
    CpuidRegs e0 = cpuid(0x80000000u);
    raw.max_ext_leaf = e0.eax;
    if (raw.max_ext_leaf >= 0x80000001u)
        raw.l80000001 = cpuid(0x80000001u);
    if (raw.max_ext_leaf >= 0x80000004u) {
        std::string brand;
        for (std::uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; ++leaf) {
            CpuidRegs r = cpuid(leaf);
            brand += regs_to_string({r.eax, r.ebx, r.ecx, r.edx});
        }
        raw.brand = trim(brand);
    }
    raw.has_xgetbv = bit(raw.l1.ecx, 27);  // OSXSAVE
    if (raw.has_xgetbv) {
#if defined(_MSC_VER)
        raw.xcr0 = _xgetbv(0);
#else
        std::uint32_t lo = 0, hi = 0;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        raw.xcr0 = (static_cast<std::uint64_t>(hi) << 32) | lo;
#endif
    }
#endif
    return raw;
}

json decode_cpu_identity(const CpuRaw& raw) {
    json j;
    j["vendor"] = raw.vendor.empty() ? "unknown" : raw.vendor;
    j["name"] = raw.brand.empty() ? "unknown" : raw.brand;
    if (raw.max_leaf >= 1) {
        const std::uint32_t eax = raw.l1.eax;
        const std::uint32_t family = (eax >> 8) & 0xFu;
        const std::uint32_t model = (eax >> 4) & 0xFu;
        const std::uint32_t ext_family = (eax >> 20) & 0xFFu;
        const std::uint32_t ext_model = (eax >> 16) & 0xFu;
        const std::uint32_t disp_family = family == 0xFu ? family + ext_family : family;
        const std::uint32_t disp_model =
            (family == 0x6u || family == 0xFu) ? (ext_model << 4) + model : model;
        j["family"] = disp_family;
        j["model"] = disp_model;
        j["stepping"] = eax & 0xFu;
    } else {
        j["family"] = unknown();
        j["model"] = unknown();
        j["stepping"] = unknown();
    }
    return j;
}

json decode_cpu_features(const CpuRaw& raw) {
    json f = json::object();
    if (raw.max_leaf < 1) {
        // Non-x86 or cpuid unavailable: nothing is claimed.
        for (const char* k : {"sse4_2", "avx", "avx2", "fma3", "f16c", "bmi1", "bmi2", "popcnt", "aes", "sha",
                              "avx512f", "avx512bw", "avx512vl", "avx512_vnni", "avx_vnni"})
            f[k] = "unknown";
        f["os_ymm_state_enabled"] = "unknown";
        f["os_zmm_state_enabled"] = "unknown";
        return f;
    }
    const bool os_ymm = raw.has_xgetbv && (raw.xcr0 & 0x6u) == 0x6u;
    const bool os_zmm = os_ymm && (raw.xcr0 & 0xE0u) == 0xE0u;
    const auto& l1 = raw.l1;
    const auto& l7 = raw.l7;
    const bool have7 = raw.max_leaf >= 7;
    // A feature is reported true only when both the CPU bit and OS state support are present.
    f["sse4_2"] = bit(l1.ecx, 20);
    f["popcnt"] = bit(l1.ecx, 23);
    f["aes"] = bit(l1.ecx, 25);
    f["avx"] = bit(l1.ecx, 28) && os_ymm;
    f["fma3"] = bit(l1.ecx, 12) && os_ymm;
    f["f16c"] = bit(l1.ecx, 29) && os_ymm;
    f["avx2"] = have7 && bit(l7.ebx, 5) && os_ymm;
    f["bmi1"] = have7 && bit(l7.ebx, 3);
    f["bmi2"] = have7 && bit(l7.ebx, 8);
    f["sha"] = have7 && bit(l7.ebx, 29);
    f["avx512f"] = have7 && bit(l7.ebx, 16) && os_zmm;
    f["avx512bw"] = have7 && bit(l7.ebx, 30) && os_zmm;
    f["avx512vl"] = have7 && bit(l7.ebx, 31) && os_zmm;
    f["avx512_vnni"] = have7 && bit(l7.ecx, 11) && os_zmm;
    f["avx_vnni"] = have7 && l7.eax >= 1 && bit(raw.l7_1.eax, 4) && os_ymm;
    f["os_ymm_state_enabled"] = os_ymm;
    f["os_zmm_state_enabled"] = os_zmm;
    return f;
}

json cpu_report() {
    const CpuRaw raw = read_cpu_raw();
    json j = decode_cpu_identity(raw);
    j["features"] = decode_cpu_features(raw);
    j["physical_cores"] = unknown();
    j["logical_processors"] = unknown();
    j["nominal_mhz"] = unknown();
#ifdef _WIN32
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<char> buf(len);
        if (GetLogicalProcessorInformationEx(
                RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
                &len)) {
            std::uint32_t cores = 0, logical = 0;
            for (DWORD off = 0; off < len;) {
                auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
                if (p->Size == 0)
                    break;
                if (p->Relationship == RelationProcessorCore) {
                    ++cores;
                    for (WORD g = 0; g < p->Processor.GroupCount; ++g)
                        logical += static_cast<std::uint32_t>(
                            std::popcount(static_cast<std::uint64_t>(p->Processor.GroupMask[g].Mask)));
                }
                off += p->Size;
            }
            if (cores > 0) {
                j["physical_cores"] = cores;
                j["logical_processors"] = logical;
            }
        }
    }
    DWORD mhz = 0, sz = sizeof(mhz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz",
                     RRF_RT_REG_DWORD, nullptr, &mhz, &sz) == ERROR_SUCCESS)
        j["nominal_mhz"] = static_cast<std::uint32_t>(mhz);
#elif defined(__linux__)
    // /proc/cpuinfo: one block per logical processor. Physical cores = distinct (physical id, core id) pairs
    // (kernel-reported topology); when the kernel does not expose them (many ARM/VM kernels) the core count
    // stays "unknown" rather than being guessed from the logical count.
    {
        std::ifstream in("/proc/cpuinfo");
        std::string line, model_name;
        std::uint32_t logical = 0;
        std::set<std::pair<std::string, std::string>> cores;
        std::string phys = "0", core;
        bool have_core = false;
        double mhz = 0;
        auto flush_block = [&] {
            if (have_core)
                cores.insert({phys, core});
            phys = "0";
            core.clear();
            have_core = false;
        };
        while (std::getline(in, line)) {
            if (line.empty()) {
                flush_block();
                continue;
            }
            const auto c = line.find(':');
            if (c == std::string::npos)
                continue;
            const std::string k = trim_ws(line.substr(0, c)), v = trim_ws(line.substr(c + 1));
            if (k == "processor")
                ++logical;
            else if (k == "model name" || k == "Model Name")
                model_name = model_name.empty() ? v : model_name;
            else if (k == "physical id")
                phys = v;
            else if (k == "core id") {
                core = v;
                have_core = true;
            } else if (k == "cpu MHz" && mhz == 0)
                mhz = std::strtod(v.c_str(), nullptr);
        }
        flush_block();
        if (logical > 0)
            j["logical_processors"] = logical;
        if (!cores.empty())
            j["physical_cores"] = static_cast<std::uint32_t>(cores.size());
        if (mhz > 0)
            j["nominal_mhz"] = static_cast<std::uint32_t>(mhz);
        if (j["name"] == "unknown" && !model_name.empty())
            j["name"] = model_name;
    }
#elif defined(__APPLE__)
    {
        auto sysctl_u32 = [](const char* name) -> std::uint32_t {
            std::uint32_t v = 0;
            std::size_t len = sizeof(v);
            return sysctlbyname(name, &v, &len, nullptr, 0) == 0 ? v : 0u;
        };
        if (const auto p = sysctl_u32("hw.physicalcpu"))
            j["physical_cores"] = p;
        if (const auto l = sysctl_u32("hw.logicalcpu"))
            j["logical_processors"] = l;
        char brand[256]{};
        std::size_t len = sizeof(brand);
        if (j["name"] == "unknown" && sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) == 0)
            j["name"] = std::string(brand);
    }
#endif
    return j;
}

}  // namespace hcs::hardware
