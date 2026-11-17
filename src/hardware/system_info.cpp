#include <string>
#include <vector>

#include "hw.hpp"
#include "win_util.hpp"

namespace hcs::hardware {

json system_report() {
    json os = {{"name", "unknown"},         {"version", "unknown"},
               {"build", "unknown"},        {"ubr", "unknown"},
               {"display_version", "unknown"}, {"product_name_registry", "unknown"},
               {"architecture", "unknown"}};
    json mem = {{"total_physical_bytes", unknown()},     {"available_physical_bytes", unknown()},
                {"installed_physical_bytes", unknown()}, {"commit_limit_bytes", unknown()},
                {"commit_available_bytes", unknown()},   {"memory_load_percent", unknown()},
                {"channel_configuration", unknown()},    {"bios_uma_reservation", unknown()}};
#ifdef _WIN32
    os["name"] = "windows";
#if defined(_M_ARM64)
    os["architecture"] = "arm64";
#else
    os["architecture"] = "x64";
#endif
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) {
            OSVERSIONINFOW v{};
            v.dwOSVersionInfoSize = sizeof(v);
            if (fn(&v) == 0) {
                os["version"] = std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion);
                os["build"] = static_cast<std::uint32_t>(v.dwBuildNumber);
            }
        }
    }
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    os["ubr"] = win::reg_dword(HKEY_LOCAL_MACHINE, cv, L"UBR");
    os["display_version"] = win::reg_string(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion");
    os["product_name_registry"] = win::reg_string(HKEY_LOCAL_MACHINE, cv, L"ProductName");

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        mem["total_physical_bytes"] = static_cast<std::uint64_t>(ms.ullTotalPhys);
        mem["available_physical_bytes"] = static_cast<std::uint64_t>(ms.ullAvailPhys);
        mem["commit_limit_bytes"] = static_cast<std::uint64_t>(ms.ullTotalPageFile);
        mem["commit_available_bytes"] = static_cast<std::uint64_t>(ms.ullAvailPageFile);
        mem["memory_load_percent"] = static_cast<std::uint32_t>(ms.dwMemoryLoad);
    }
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb))
        mem["installed_physical_bytes"] = static_cast<std::uint64_t>(kb) * 1024u;
#endif
    return {{"os", os}, {"memory", mem}};
}

}  // namespace hcs::hardware
