#include "hcs/gui/gui_assets.hpp"

namespace hcs::gui {

namespace {

// Strict: scripts, styles and images only from this origin (data:/blob: for images the user picked or the server
// generated). No inline script or style, no eval, no framing, no forms posting anywhere, no plugins.
constexpr std::string_view kCsp =
    "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; connect-src 'self'; "
    "font-src 'none'; media-src 'none'; object-src 'none'; frame-src 'none'; worker-src 'none'; "
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

void add_security_headers(GuiResponse& r) {
    r.headers.emplace_back("Content-Security-Policy", std::string(kCsp));
    r.headers.emplace_back("X-Frame-Options", "DENY");
    r.headers.emplace_back("Referrer-Policy", "no-referrer");
    r.headers.emplace_back("Cross-Origin-Opener-Policy", "same-origin");
    r.headers.emplace_back("Cross-Origin-Resource-Policy", "same-origin");
    r.headers.emplace_back("Permissions-Policy", "camera=(), microphone=(), geolocation=(), payment=(), usb=()");
}

GuiResponse make_error(int status, std::string_view text) {
    GuiResponse r;
    r.status = status;
    r.content_type = "text/plain; charset=utf-8";
    r.body = text;
    add_security_headers(r);
    return r;
}

// Asset names are [A-Za-z0-9._-] only, no leading dot, no "..": anything else (separators, percent signs, drive
// letters, NUL, backslash) is a malformed/traversal attempt and is rejected before any lookup.
bool valid_asset_name(std::string_view n) noexcept {
    if (n.empty() || n.size() > 64 || n.front() == '.') return false;
    for (std::size_t i = 0; i < n.size(); ++i) {
        const char c = n[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
        if (c == '.' && i + 1 < n.size() && n[i + 1] == '.') return false;
    }
    return true;
}

const detail::EmbeddedAsset* find_asset(std::string_view name) noexcept {
    for (std::size_t i = 0; i < detail::kEmbeddedAssetCount; ++i)
        if (name == detail::kEmbeddedAssets[i].name) return &detail::kEmbeddedAssets[i];
    return nullptr;
}

}  // namespace

std::string_view content_security_policy() noexcept { return kCsp; }

bool handles_path(std::string_view path) noexcept { return path == "/" || path.rfind("/ui/", 0) == 0 || path == "/ui"; }

std::vector<std::string_view> asset_names() {
    std::vector<std::string_view> v;
    for (std::size_t i = 0; i < detail::kEmbeddedAssetCount; ++i) v.emplace_back(detail::kEmbeddedAssets[i].name);
    return v;
}

std::optional<GuiResponse> serve(std::string_view method, std::string_view path) {
    if (!handles_path(path)) return std::nullopt;
    if (method != "GET" && method != "HEAD") {
        GuiResponse r = make_error(405, "method not allowed\n");
        r.headers.emplace_back("Allow", "GET, HEAD");
        return r;
    }
    std::string_view name;
    if (path == "/") {
        name = "index.html";
    } else {
        if (path == "/ui") return make_error(404, "not found\n");
        name = path.substr(4);  // after "/ui/"
        if (!valid_asset_name(name)) return make_error(400, "bad path\n");
    }
    const auto* a = find_asset(name);
    if (!a) return make_error(404, "not found\n");
    GuiResponse r;
    r.status = 200;
    r.content_type = a->content_type;
    if (method == "GET") r.body = std::string_view(reinterpret_cast<const char*>(a->data), a->size);
    add_security_headers(r);
    return r;
}

}  // namespace hcs::gui
