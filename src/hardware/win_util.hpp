#pragma once
// Small Windows helpers shared by the hardware module sources.
#ifdef _WIN32
#include <windows.h>

#include <string>

#include "hw.hpp"

namespace hcs::hardware::win {

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline json reg_string(HKEY root, const wchar_t* key, const wchar_t* name) {
    DWORD sz = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &sz) != ERROR_SUCCESS || sz == 0)
        return unknown();
    std::wstring w(sz / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, w.data(), &sz) != ERROR_SUCCESS) return unknown();
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return narrow(w);
}

inline json reg_dword(HKEY root, const wchar_t* key, const wchar_t* name) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return unknown();
    return static_cast<std::uint32_t>(v);
}

}  // namespace hcs::hardware::win
#endif
