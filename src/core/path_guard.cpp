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
