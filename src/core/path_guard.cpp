#include "hcs/path_guard.hpp"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace hcs {

namespace fs = std::filesystem;

namespace {

fs::path::string_type fold(const fs::path::string_type& s) {
#ifdef _WIN32
    fs::path::string_type r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return r;
#else
    return s;
#endif
}

bool is_within(const fs::path& root, const fs::path& target) {
    auto r = root.begin();
    auto t = target.begin();
    for (; r != root.end(); ++r, ++t) {
        if (t == target.end()) return false;
        if (r->empty()) continue;  // trailing separator artefact
        if (fold(r->native()) != fold(t->native())) return false;
    }
    return true;
}

#ifdef _WIN32
// Win32 resolves these names to DOS devices in ANY directory (also with an extension or trailing dots/spaces):
// "<approved root>/NUL.png" is the NUL device and "<root>/CON" blocks on console input. They are never model or
// image files, so the guard rejects them (M10 audit D5).
bool is_reserved_device_stem(std::wstring stem) {
    for (auto& c : stem) c = static_cast<wchar_t>(std::towupper(c));
    while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" ||
        stem == L"CONOUT$" || stem == L"CLOCK$")
        return true;
    if (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0)) {
        const wchar_t d = stem[3];
        // COM1-COM9, plus the superscript digits (U+00B9, U+00B2, U+00B3) that Windows also reserves.
        return (d >= L'1' && d <= L'9') || d == 0x00B9 || d == 0x00B2 || d == 0x00B3;
    }
    return false;
}

Result<void> check_component_win(const fs::path::string_type& comp) {
    if (comp.empty() || comp == L"." || comp == L"..") return {};
    if (comp.size() > 255) return make_error(ErrorCode::SecurityViolation, "path component is too long");
    for (wchar_t c : comp) {
        if (c < 0x20 || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'*' || c == L'?')
            return make_error(ErrorCode::SecurityViolation, "path component contains a reserved or wildcard character");
    }
    // Win32 silently strips trailing dots/spaces: "model.gguf." names "model.gguf" (and "con." names CON).
    if (comp.back() == L'.' || comp.back() == L' ')
        return make_error(ErrorCode::SecurityViolation, "path component ends with a dot or space");
    // The device name is the part before the FIRST dot ("nul.tar.gz" is NUL), ignoring trailing spaces.
    const auto dot = comp.find(L'.');
    std::wstring stem = dot == std::wstring::npos ? comp : comp.substr(0, dot);
    if (is_reserved_device_stem(std::move(stem)))
        return make_error(ErrorCode::SecurityViolation, "reserved Windows device name in path");
    return {};
}
#endif

Result<void> check_raw(const fs::path& p) {
    if (p.empty()) return make_error(ErrorCode::InvalidArgument, "empty path");
    const auto& n = p.native();
    if (n.find(fs::path::value_type('\0')) != fs::path::string_type::npos) {
        return make_error(ErrorCode::SecurityViolation, "path contains NUL");
    }
    for (const auto& part : p) {
        if (part == "..") return make_error(ErrorCode::SecurityViolation, "path contains '..' component");
    }
#ifdef _WIN32
    if (n.rfind(L"\\\\?\\", 0) == 0 || n.rfind(L"\\\\.\\", 0) == 0 || n.rfind(L"//?/", 0) == 0 ||
        n.rfind(L"//./", 0) == 0) {
        return make_error(ErrorCode::SecurityViolation, "device/namespace paths are not allowed");
    }
    for (std::size_t i = 0; i < n.size(); ++i) {
        if (n[i] == L':' && !(i == 1 && std::iswalpha(n[0]))) {
            return make_error(ErrorCode::SecurityViolation, "alternate data streams / stray ':' not allowed");
        }
    }
    for (const auto& part : p.relative_path()) {
        if (auto r = check_component_win(part.native()); !r) return r;
    }
#endif
    return {};
}

}  // namespace

Result<void> PathGuard::add_root(const fs::path& root) {
    if (auto r = check_raw(root); !r) return r;
    std::error_code ec;
    fs::path canon = fs::canonical(root, ec);
    if (ec) return make_error(ErrorCode::NotFound, "approved root does not exist: " + root.string());
    if (!fs::is_directory(canon, ec)) {
        return make_error(ErrorCode::InvalidArgument, "approved root is not a directory: " + root.string());
    }
    if (std::find(roots_.begin(), roots_.end(), canon) == roots_.end()) roots_.push_back(std::move(canon));
    return {};
}

Result<fs::path> PathGuard::resolve(const fs::path& candidate) const {
    if (auto r = check_raw(candidate); !r) return r.error();
    if (roots_.empty()) return make_error(ErrorCode::SecurityViolation, "no approved roots configured");
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::absolute(candidate, ec), ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot canonicalise path: " + ec.message());
    for (const auto& root : roots_) {
        if (is_within(root, canon)) return canon;
    }
    return make_error(ErrorCode::SecurityViolation, "path is outside approved roots: " + candidate.string());
}

}  // namespace hcs
