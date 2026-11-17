#pragma once
// Embedded local web UI (ADR-009). Pure function: maps (method, path) to the bytes, content type and security
// headers of a compiled-in asset. It knows nothing about the HTTP framework, so the API layer only needs a tiny hook
// (see docs/gui.md):
//
//   if (auto g = hcs::gui::serve(req.method, req.path)) {
//       resp.status = g->status; resp.content_type = g->content_type; resp.headers = g->headers;
//       resp.body = std::string(g->body);   // g->body views static storage
//       return resp;                        // place AFTER the Host/Origin checks, BEFORE bearer auth
//   }
//
// The assets carry no secret; the API token is never embedded - the page reads it from the URL fragment.
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hcs::gui {

struct GuiResponse {
    int status = 200;
    std::string content_type;
    // Security headers (CSP, framing, referrer, COOP/CORP, permissions). Cache-Control and X-Content-Type-Options are
    // intentionally NOT included: ApiHandler::handle() appends them to every response.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string_view body;  // always views static storage (asset bytes or a literal error text; empty for HEAD)
};

// True when `path` belongs to the UI namespace: "/" or anything under "/ui/".
bool handles_path(std::string_view path) noexcept;

// Returns nullopt when the path is outside the UI namespace (caller continues with normal API routing).
// Inside the namespace it always answers: 200 asset, 400 malformed/traversal path, 404 unknown asset,
// 405 for methods other than GET/HEAD. `path` is the already-decoded request path without query string.
std::optional<GuiResponse> serve(std::string_view method, std::string_view path);

// The exact Content-Security-Policy sent with every UI response.
std::string_view content_security_policy() noexcept;

// Names of embedded assets, as served under /ui/<name> (index.html is also served at "/").
std::vector<std::string_view> asset_names();

namespace detail {
struct EmbeddedAsset {
    const char* name;
    const char* content_type;
    const unsigned char* data;
    std::size_t size;
};
// Defined in the CMake-generated translation unit.
extern const EmbeddedAsset kEmbeddedAssets[];
extern const std::size_t kEmbeddedAssetCount;
}  // namespace detail

}  // namespace hcs::gui
