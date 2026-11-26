#include <cstdio>
#include <string>
#include <vector>

#include <optional>

#include "hcs/hardware/memory_probe.hpp"
#include "hw.hpp"
#include "win_util.hpp"

#ifdef _WIN32
#include <dxgi1_4.h>
#endif

namespace hcs::hardware {

#ifdef _WIN32
namespace {

std::string hex4(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%04X", v);
    return b;
}

std::string version_quad(std::int64_t v) {
    const auto u = static_cast<std::uint64_t>(v);
    return std::to_string((u >> 48) & 0xFFFF) + "." + std::to_string((u >> 32) & 0xFFFF) + "." +
           std::to_string((u >> 16) & 0xFFFF) + "." + std::to_string(u & 0xFFFF);
}

json segment_budget(IDXGIAdapter3* a3, DXGI_MEMORY_SEGMENT_GROUP group) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!a3 || FAILED(a3->QueryVideoMemoryInfo(0, group, &info)))
        return unknown();
    return {{"budget_bytes", static_cast<std::uint64_t>(info.Budget)},
            {"current_usage_bytes", static_cast<std::uint64_t>(info.CurrentUsage)},
            {"available_for_reservation_bytes", static_cast<std::uint64_t>(info.AvailableForReservation)}};
}

json dxgi_adapters() {
    using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE lib = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib)
        return {
            {"available", false}, {"reason", "dxgi.dll could not be loaded"}, {"adapters", json::array()}};
    auto create =
        reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(lib, "CreateDXGIFactory1")));
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        FreeLibrary(lib);
        return {{"available", false}, {"reason", "CreateDXGIFactory1 failed"}, {"adapters", json::array()}};
    }
    json adapters = json::array();
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(ad->GetDesc1(&d))) {
            json a;
            a["index"] = i;
            a["name"] = win::narrow(d.Description);
            a["vendor_id"] = hex4(d.VendorId);
            a["device_id"] = hex4(d.DeviceId);
            a["subsystem_id"] = hex4(d.SubSysId);
            a["revision"] = static_cast<std::uint32_t>(d.Revision);
            a["software_adapter"] = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            // Dedicated video memory on an iGPU is the firmware (BIOS) carve-out as seen by DXGI.
            a["dedicated_video_memory_bytes"] = static_cast<std::uint64_t>(d.DedicatedVideoMemory);
            a["dedicated_system_memory_bytes"] = static_cast<std::uint64_t>(d.DedicatedSystemMemory);
            a["shared_system_memory_bytes"] = static_cast<std::uint64_t>(d.SharedSystemMemory);
            LARGE_INTEGER umd{};
            if (SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                a["umd_version"] = version_quad(umd.QuadPart);
            else
                a["umd_version"] = unknown();
            IDXGIAdapter3* a3 = nullptr;
            if (SUCCEEDED(ad->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) && a3) {
                a["memory_local_segment"] = segment_budget(a3, DXGI_MEMORY_SEGMENT_GROUP_LOCAL);
                a["memory_non_local_segment"] = segment_budget(a3, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL);
                a3->Release();
            } else {
                a["memory_local_segment"] = unknown();
                a["memory_non_local_segment"] = unknown();
            }
            adapters.push_back(std::move(a));
        }
        ad->Release();
        ad = nullptr;
    }
    factory->Release();
    // dxgi.dll is intentionally left loaded (cheap; avoids unload races with driver threads).
    return {{"available", true}, {"adapters", adapters}};
}

json registry_display_drivers() {
    json out = json::array();
    const wchar_t* base =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD n = 256;
        const LONG rc = RegEnumKeyExW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS)
            continue;
        const std::wstring sub = std::wstring(base) + L"\\" + std::wstring(name, n);
        json desc = win::reg_string(HKEY_LOCAL_MACHINE, sub.c_str(), L"DriverDesc");
        if (desc.is_string() && desc.get<std::string>() == "unknown")
            continue;  // not a display adapter entry
        json e;
        e["description"] = desc;
        e["provider"] = win::reg_string(HKEY_LOCAL_MACHINE, sub.c_str(), L"ProviderName");
        e["driver_version"] = win::reg_string(HKEY_LOCAL_MACHINE, sub.c_str(), L"DriverVersion");
        e["driver_date"] = win::reg_string(HKEY_LOCAL_MACHINE, sub.c_str(), L"DriverDate");
        e["matching_device_id"] = win::reg_string(HKEY_LOCAL_MACHINE, sub.c_str(), L"MatchingDeviceId");
        out.push_back(std::move(e));
    }
    RegCloseKey(h);
    return out;
}

}  // namespace
#endif

json gpu_report() {
    json j;
#ifdef _WIN32
    json dx = dxgi_adapters();
    j["dxgi_available"] = dx["available"];
    if (dx.contains("reason"))
        j["dxgi_reason"] = dx["reason"];
    j["adapters"] = dx["adapters"];
    j["display_driver_registry"] = registry_display_drivers();
#else
    j["dxgi_available"] = false;
    j["adapters"] = json::array();
    j["display_driver_registry"] = json::array();
#endif
    return j;
}

std::optional<std::uint64_t> sample_gpu_heap_headroom() {
#ifdef _WIN32
    const json dx = dxgi_adapters();
    if (!dx.value("available", false))
        return std::nullopt;
    auto segment_headroom = [](const json& a, const char* key) -> std::optional<std::uint64_t> {
        if (!a.contains(key) || !a[key].is_object())
            return std::nullopt;
        const auto& s = a[key];
        if (!s.contains("budget_bytes") || !s.contains("current_usage_bytes"))
            return std::nullopt;
        const auto b = s["budget_bytes"].get<std::uint64_t>();
        const auto u = s["current_usage_bytes"].get<std::uint64_t>();
        return b > u ? b - u : 0;
    };
    std::optional<std::uint64_t> best;
    for (const auto& a : dx["adapters"]) {
        if (a.value("software_adapter", false))
            continue;
        const auto local = segment_headroom(a, "memory_local_segment");
        const auto non_local = segment_headroom(a, "memory_non_local_segment");
        if (!local && !non_local)
            continue;
        const std::uint64_t total = local.value_or(0) + non_local.value_or(0);
        if (!best || total > *best)
            best = total;
    }
    return best;
#else
    return std::nullopt;
#endif
}

}  // namespace hcs::hardware