#include <string>
#include <vector>

#include "hw.hpp"
#include "win_util.hpp"

#if !defined(_WIN32)
#include <sys/utsname.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <fstream>
#include <sstream>
#endif
#endif

namespace hcs::hardware {

#if defined(__linux__)
namespace {
// KEY=value / KEY="value" lines of /etc/os-release (the systemd-defined identification file).
std::string os_release_value(const char* key) {
    std::ifstream in("/etc/os-release");
    if (!in)
        in.open("/usr/lib/os-release");
    std::string line;
    const std::string prefix = std::string(key) + "=";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) != 0)
            continue;
        std::string v = line.substr(prefix.size());
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}
std::uint64_t meminfo_kb(const std::string& text, const char* key) {
    const std::string k = std::string(key) + ":";
    const auto p = text.find(k);
    if (p == std::string::npos || (p != 0 && text[p - 1] != '\n'))
        return 0;
    return std::strtoull(text.c_str() + p + k.size(), nullptr, 10);
}
}  // namespace
#endif

json system_report() {
    json os = {
        {"name", "unknown"},        {"version", "unknown"},         {"build", "unknown"},
        {"ubr", "unknown"},         {"display_version", "unknown"}, {"product_name_registry", "unknown"},
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
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) {
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
#elif defined(__linux__) || defined(__APPLE__)
    struct utsname u{};
    if (uname(&u) == 0) {
#if defined(__APPLE__)
        os["name"] = "macos";
#else
        os["name"] = "linux";
#endif
        os["version"] = std::string(u.release);  // kernel release (e.g. 6.8.0-45-generic)
        os["build"] = std::string(u.version);    // kernel build string
        const std::string m = u.machine;
        os["architecture"] = (m == "x86_64" || m == "amd64")    ? "x64"
                             : (m == "aarch64" || m == "arm64") ? "arm64"
                                                                : m;
    }
#if defined(__linux__)
    if (const std::string pretty = os_release_value("PRETTY_NAME"); !pretty.empty())
        os["product_name_registry"] =
            pretty;  // field name is historical (Windows registry); holds the distro name
    if (const std::string ver = os_release_value("VERSION_ID"); !ver.empty())
        os["display_version"] = ver;
    std::ifstream mi("/proc/meminfo");
    if (mi) {
        std::stringstream ss;
        ss << mi.rdbuf();
        const std::string t = ss.str();
        const auto total = meminfo_kb(t, "MemTotal"), avail = meminfo_kb(t, "MemAvailable");
        const auto climit = meminfo_kb(t, "CommitLimit"), cas = meminfo_kb(t, "Committed_AS");
        if (total > 0) {
            mem["total_physical_bytes"] = total * 1024ull;
            mem["available_physical_bytes"] = avail * 1024ull;
            mem["memory_load_percent"] = static_cast<std::uint32_t>(100ull - (avail * 100ull) / total);
        }
        if (climit > 0) {
            mem["commit_limit_bytes"] = climit * 1024ull;
            mem["commit_available_bytes"] = (climit > cas ? climit - cas : 0ull) * 1024ull;
        }
    }
    // installed_physical_bytes / channel_configuration / bios_uma_reservation need DMI (root) and stay
    // unknown.
#else
    std::uint64_t memsize = 0;
    std::size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0 && memsize > 0) {
        mem["total_physical_bytes"] = memsize;
        mem["installed_physical_bytes"] = memsize;  // unified memory: installed == total
    }
#endif
#endif
    return {{"os", os}, {"memory", mem}};
}

}  // namespace hcs::hardware
