#include "hcs/model_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

#include "hcs/checked_math.hpp"
#include "hcs/gguf_probe.hpp"

namespace hcs {

using nlohmann::json;
namespace fs = std::filesystem;

std::string_view to_string(ModelFormat f) noexcept {
    switch (f) {
        case ModelFormat::Unknown: return "unknown";
        case ModelFormat::Gguf: return "gguf";
        case ModelFormat::Safetensors: return "safetensors";
        case ModelFormat::HcsModel: return "hcsmodel";
    }
    return "unknown";
}
std::string_view to_string(ModelKind k) noexcept {
    switch (k) {
        case ModelKind::Unknown: return "unknown";
        case ModelKind::Text: return "text";
        case ModelKind::Vision: return "vision";
        case ModelKind::Image: return "image";
    }
    return "unknown";
}

namespace {
ModelFormat parse_format(std::string_view s) {
    for (auto f : {ModelFormat::Gguf, ModelFormat::Safetensors, ModelFormat::HcsModel}) {
        if (to_string(f) == s) return f;
    }
    return ModelFormat::Unknown;
}
ModelKind parse_kind(std::string_view s) {
    for (auto k : {ModelKind::Text, ModelKind::Vision, ModelKind::Image}) {
        if (to_string(k) == s) return k;
    }
    return ModelKind::Unknown;
}

bool state_allows_removal(ModelState s) {
    switch (s) {
        case ModelState::Validating:
        case ModelState::Loading:
        case ModelState::Loaded:
        case ModelState::Running:
        case ModelState::Unloading: return false;
        default: return true;
    }
}

std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

// Reads up to n bytes; returns bytes read.
std::size_t read_head(const fs::path& p, unsigned char* buf, std::size_t n) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return 0;
    in.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(in.gcount());
}

// Returns empty string if the header is valid, else a reason.
std::string validate_gguf_header(const fs::path& p) {
    unsigned char h[8] = {};
    if (read_head(p, h, sizeof h) < sizeof h) return "file too small for a GGUF header";
    if (std::memcmp(h, "GGUF", 4) != 0) return "invalid GGUF magic";
    const std::uint32_t version = static_cast<std::uint32_t>(h[4]) | (static_cast<std::uint32_t>(h[5]) << 8) |
                                  (static_cast<std::uint32_t>(h[6]) << 16) | (static_cast<std::uint32_t>(h[7]) << 24);
    if (version < 1 || version > 3) return "unsupported GGUF version " + std::to_string(version);
    return {};
}

std::string validate_safetensors_header(const fs::path& p, std::uint64_t file_size) {
    unsigned char h[8] = {};
    if (read_head(p, h, sizeof h) < sizeof h) return "file too small for a safetensors header";
    std::uint64_t len = 0;
    for (int i = 7; i >= 0; --i) len = (len << 8) | h[i];
    if (len == 0 || len > (100ull << 20) || !range_within(8, len, file_size)) return "invalid safetensors header length";
    return {};
}

json manifest_to_json(const ModelManifest& m) {
    return json{{"id", m.id},
                {"display_name", m.display_name},
                {"path", m.path},
                {"format", std::string(to_string(m.format))},
                {"kind", std::string(to_string(m.kind))},
                {"size_bytes", m.size_bytes},
                {"sha256", m.sha256},
                {"architecture", m.architecture},
                {"quantization", m.quantization},
                {"license", m.license}};
}
}  // namespace

bool is_valid_model_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) return false;
    auto alnum = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(static_cast<unsigned char>(id[0]))) return false;
    for (unsigned char c : id) {
        if (!(alnum(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

std::string make_model_id(std::string_view stem) {
    std::string id;
    for (unsigned char c : stem) {
        char l = static_cast<char>(std::tolower(c));
        const bool ok = (l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '.' || l == '_' || l == '-';
        id += ok ? l : '-';
    }
    while (!id.empty() && !((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9'))) id.erase(0, 1);
    if (id.size() > 64) id.resize(64);
    if (id.empty()) id = "model";
    return id;
}

Result<std::string> ModelRegistry::add_locked(ModelManifest m) {
    if (!is_valid_model_id(m.id)) return make_error(ErrorCode::InvalidArgument, "invalid model id: " + m.id);
    if (models_.find(m.id) != models_.end()) return make_error(ErrorCode::AlreadyExists, "model id exists: " + m.id);
    if (m.display_name.empty()) m.display_name = m.id;
    Entry e;
    e.lifecycle = std::make_unique<ModelLifecycle>();
    (void)e.lifecycle->transition(ModelState::Discovered);
    std::string id = m.id;
    e.manifest = std::move(m);
    models_.emplace(id, std::move(e));
    return id;
}

Result<std::string> ModelRegistry::add(ModelManifest manifest) {
    std::lock_guard lock(mutex_);
    return add_locked(std::move(manifest));
}

Result<ModelManifest> ModelRegistry::get(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = models_.find(id);
    if (it == models_.end()) return make_error(ErrorCode::NotFound, "unknown model: " + std::string(id));
    return it->second.manifest;
}

std::vector<ModelManifest> ModelRegistry::list() const {
    std::lock_guard lock(mutex_);
    std::vector<ModelManifest> out;
    for (const auto& [k, e] : models_) out.push_back(e.manifest);
    return out;
}

std::size_t ModelRegistry::size() const {
    std::lock_guard lock(mutex_);
    return models_.size();
}

Result<void> ModelRegistry::remove(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto it = models_.find(id);
    if (it == models_.end()) return make_error(ErrorCode::NotFound, "unknown model: " + std::string(id));
    if (!state_allows_removal(it->second.lifecycle->state())) {
        return make_error(ErrorCode::InvalidState, "model is in use; unload it first: " + std::string(id));
    }
    models_.erase(it);
    return {};
}

Result<ModelState> ModelRegistry::state(std::string_view id) const {
    std::lock_guard lock(mutex_);
    auto it = models_.find(id);
    if (it == models_.end()) return make_error(ErrorCode::NotFound, "unknown model: " + std::string(id));
    return it->second.lifecycle->state();
}

Result<void> ModelRegistry::transition(std::string_view id, ModelState to) {
    std::lock_guard lock(mutex_);
    auto it = models_.find(id);
    if (it == models_.end()) return make_error(ErrorCode::NotFound, "unknown model: " + std::string(id));
    return it->second.lifecycle->transition(to);
}

Result<void> ModelRegistry::fail(std::string_view id, FailureKind kind, std::string reason) {
    std::lock_guard lock(mutex_);
    auto it = models_.find(id);
    if (it == models_.end()) return make_error(ErrorCode::NotFound, "unknown model: " + std::string(id));
    return it->second.lifecycle->fail(kind, std::move(reason));
}

Result<ScanReport> ModelRegistry::scan(const fs::path& root, const PathGuard& guard) {
    auto resolved_root = guard.resolve(root);
    if (!resolved_root) return resolved_root.error();
    std::error_code ec;
    if (!fs::is_directory(resolved_root.value(), ec)) {
        return make_error(ErrorCode::NotFound, "not a directory: " + root.string());
    }

    ScanReport report;
    fs::recursive_directory_iterator it(resolved_root.value(), fs::directory_options::skip_permission_denied, ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot enumerate " + root.string() + ": " + ec.message());
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) { report.messages.push_back("enumeration error: " + ec.message()); break; }
        if (it.depth() >= 8) { it.disable_recursion_pending(); }
        // Every entry (file, directory, symlink, junction) must canonicalise inside an approved
        // root; otherwise it is skipped and, for directories, not descended into.
        auto canon = guard.resolve(it->path());
        if (!canon) {
            ++report.skipped;
            report.messages.push_back("skipped (outside approved roots): " + it->path().string());
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lower_ext(it->path());
        const ModelFormat fmt = ext == ".gguf"          ? ModelFormat::Gguf
                                : ext == ".safetensors" ? ModelFormat::Safetensors
                                                        : ModelFormat::Unknown;
        if (fmt == ModelFormat::Unknown) continue;
        const std::uint64_t size = fs::file_size(canon.value(), ec);
        if (ec) { ++report.skipped; report.messages.push_back("skipped (cannot stat): " + it->path().string()); continue; }
        const std::string reason = fmt == ModelFormat::Gguf ? validate_gguf_header(canon.value())
                                                            : validate_safetensors_header(canon.value(), size);
        if (!reason.empty()) {
            ++report.skipped;
            report.messages.push_back("skipped (" + reason + "): " + it->path().string());
            continue;
        }

        ModelManifest m;
        m.path = canon.value().string();
        m.format = fmt;
        m.size_bytes = size;
        // GGUF is the only format the registry can inspect cheaply today: read the metadata and the
        // tensor-info names (bounded) so `kind`/`architecture` reflect the file, not the extension.
        if (fmt == ModelFormat::Gguf) {
            const auto probe = probe_gguf_file(canon.value());
            m.kind = probe.kind == ModelKind::Unknown ? ModelKind::Text : probe.kind;
            if (!probe.architecture.empty()) m.architecture = probe.architecture;
            if (!probe.complete) {
                report.messages.push_back("partial GGUF probe (" +
                                          (probe.reason.empty() ? std::string("prefix too short") : probe.reason) + "): " +
                                          it->path().string());
            }
        } else {
            m.kind = ModelKind::Unknown;  // refined by engine adapters
        }

        std::lock_guard lock(mutex_);
        auto same_path = std::find_if(models_.begin(), models_.end(),
                                      [&](const auto& kv) { return kv.second.manifest.path == m.path; });
        if (same_path != models_.end()) {
            same_path->second.manifest.size_bytes = size;
            ++report.updated;
            continue;
        }
        const std::string base = make_model_id(canon.value().stem().string());
        std::string id = base;
        for (int n = 2; models_.count(id) != 0; ++n) {
            const std::string suffix = "-" + std::to_string(n);
            id = base.substr(0, 64 - suffix.size()) + suffix;
        }
        m.id = id;
        m.display_name = canon.value().stem().string();
        if (add_locked(std::move(m))) ++report.added;
    }
    return report;
}

std::string ModelRegistry::to_json() const {
    std::lock_guard lock(mutex_);
    json arr = json::array();
    for (const auto& [k, e] : models_) arr.push_back(manifest_to_json(e.manifest));
    return json{{"schema_version", 1}, {"models", arr}}.dump(2) + "\n";
}

Result<void> ModelRegistry::load_json(std::string_view text) {
    json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("models") || !j["models"].is_array()) {
        return make_error(ErrorCode::InvalidArgument, "registry file is not valid");
    }
    if (j.value("schema_version", 0) != 1) {
        return make_error(ErrorCode::Unsupported, "unsupported registry schema_version");
    }
    try {
        std::lock_guard lock(mutex_);
        for (const auto& e : j["models"]) {
            ModelManifest m;
            m.id = e.at("id").get<std::string>();
            m.display_name = e.value("display_name", m.id);
            m.path = e.at("path").get<std::string>();
            m.format = parse_format(e.value("format", "unknown"));
            m.kind = parse_kind(e.value("kind", "unknown"));
            m.size_bytes = e.value("size_bytes", std::uint64_t{0});
            m.sha256 = e.value("sha256", "");
            m.architecture = e.value("architecture", "unknown");
            m.quantization = e.value("quantization", "unknown");
            m.license = e.value("license", "unknown");
            if (models_.count(m.id) != 0) continue;
            if (auto r = add_locked(std::move(m)); !r && r.error().code != ErrorCode::AlreadyExists) return r.error();
        }
    } catch (const json::exception& ex) {
        return make_error(ErrorCode::InvalidArgument, std::string("registry entry invalid: ") + ex.what());
    }
    return {};
}

}  // namespace hcs
