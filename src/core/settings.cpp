#include "hcs/settings.hpp"

#include <nlohmann/json.hpp>

#include "hcs/fs_util.hpp"
#include "hcs/logger.hpp"

namespace hcs {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {
// Strict dotted-quad parse: exactly four decimal octets 0..255, no signs, spaces or leading junk. Returns the
// first octet through `first` so the caller can test the 127.0.0.0/8 range.
bool parse_ipv4_literal(std::string_view h, int& first) noexcept {
    int octets = 0;
    std::size_t i = 0;
    while (i <= h.size() && octets < 4) {
        const std::size_t start = i;
        int v = 0;
        while (i < h.size() && h[i] >= '0' && h[i] <= '9') {
            v = v * 10 + (h[i] - '0');
            if (v > 255) return false;
            ++i;
        }
        if (i == start || i - start > 3) return false;
        if (i - start > 1 && h[start] == '0') return false;  // "0127" is octal for some resolvers: refuse the ambiguity
        if (octets == 0) first = v;
        ++octets;
        if (octets == 4) return i == h.size();
        if (i >= h.size() || h[i] != '.') return false;
        ++i;
    }
    return false;
}
}  // namespace

// Only real loopback literals qualify: "127.foo.invalid" or "127.example.net" are DNS names that may resolve to a LAN
// address, so a prefix match would bind a non-loopback interface without --lan (M10 audit D7).
bool is_loopback_host(std::string_view h) noexcept {
    if (h == "localhost" || h == "::1" || h == "[::1]") return true;
    int first = -1;
    return parse_ipv4_literal(h, first) && first == 127;
}

std::string_view to_string(AccelProfile p) noexcept {
    switch (p) {
        case AccelProfile::Auto: return "auto";
        case AccelProfile::Vulkan: return "vulkan";
        case AccelProfile::CpuOnly: return "cpu";
    }
    return "auto";
}
std::string_view to_string(MemoryProfile p) noexcept {
    switch (p) {
        case MemoryProfile::Auto: return "auto";
        case MemoryProfile::Fast: return "fast";
        case MemoryProfile::Balanced: return "balanced";
        case MemoryProfile::LowMemory: return "low_memory";
    }
    return "auto";
}

namespace {
bool parse_accel(const std::string& s, AccelProfile& out) {
    for (auto p : {AccelProfile::Auto, AccelProfile::Vulkan, AccelProfile::CpuOnly}) {
        if (to_string(p) == s) { out = p; return true; }
    }
    return false;
}
bool parse_memory(const std::string& s, MemoryProfile& out) {
    for (auto p : {MemoryProfile::Auto, MemoryProfile::Fast, MemoryProfile::Balanced, MemoryProfile::LowMemory}) {
        if (to_string(p) == s) { out = p; return true; }
    }
    return false;
}

// v0 -> v1: host->bind_host, model_dir -> model_roots[]
void migrate_v0_to_v1(json& j) {
    if (j.contains("host")) { j["bind_host"] = j["host"]; j.erase("host"); }
    if (j.contains("model_dir")) {
        if (j["model_dir"].is_string() && !j["model_dir"].get<std::string>().empty()) {
            j["model_roots"] = json::array({j["model_dir"]});
        }
        j.erase("model_dir");
    }
    j["schema_version"] = 1;
}
}  // namespace

Result<void> validate(const Settings& s) {
    if (s.schema_version != kSettingsSchemaVersion) {
        return make_error(ErrorCode::InvalidArgument, "unexpected schema_version");
    }
    if (s.port < 1 || s.port > 65535) return make_error(ErrorCode::InvalidArgument, "port must be 1..65535");
    if (s.bind_host.empty()) return make_error(ErrorCode::InvalidArgument, "bind_host is empty");
    if (!is_loopback_host(s.bind_host) && !s.allow_lan) {
        return make_error(ErrorCode::SecurityViolation,
                          "non-loopback bind_host requires explicit allow_lan=true (and authentication)");
    }
    if (!parse_log_level(s.log_level)) return make_error(ErrorCode::InvalidArgument, "unknown log_level");
    // Upper bound equals the llama adapter's own cap (1 Mi tokens); a larger value could only fail later at load.
    if (s.context_tokens < 128 || s.context_tokens > (1u << 20)) {
        return make_error(ErrorCode::InvalidArgument, "context_tokens must be 128..1048576");
    }
    for (const auto& r : s.model_roots) {
        if (r.empty()) return make_error(ErrorCode::InvalidArgument, "empty model root");
    }
    return {};
}

std::string settings_to_json(const Settings& s) {
    json j;
    j["schema_version"] = s.schema_version;
    j["bind_host"] = s.bind_host;
    j["port"] = s.port;
    j["allow_lan"] = s.allow_lan;
    j["accel"] = std::string(to_string(s.accel));
    j["memory"] = std::string(to_string(s.memory));
    j["model_roots"] = s.model_roots;
    j["log_level"] = s.log_level;
    j["context_tokens"] = s.context_tokens;
    return j.dump(2) + "\n";
}

Result<Settings> parse_settings(std::string_view text) {
    json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "settings file is not a valid JSON object");
    }
    try {
        int version = 0;
        if (j.contains("schema_version")) {
            if (!j["schema_version"].is_number_integer()) {
                return make_error(ErrorCode::InvalidArgument, "schema_version must be an integer");
            }
            version = j["schema_version"].get<int>();
        }
        if (version < 0) return make_error(ErrorCode::InvalidArgument, "negative schema_version");
        if (version > kSettingsSchemaVersion) {
            return make_error(ErrorCode::Unsupported,
                              "settings schema_version " + std::to_string(version) + " is newer than supported " +
                                  std::to_string(kSettingsSchemaVersion));
        }
        if (version == 0) migrate_v0_to_v1(j);

        Settings s;
        if (j.contains("bind_host")) s.bind_host = j.at("bind_host").get<std::string>();
        if (j.contains("port")) s.port = j.at("port").get<int>();
        if (j.contains("allow_lan")) s.allow_lan = j.at("allow_lan").get<bool>();
        if (j.contains("accel") && !parse_accel(j.at("accel").get<std::string>(), s.accel)) {
            return make_error(ErrorCode::InvalidArgument, "unknown accel profile");
        }
        if (j.contains("memory") && !parse_memory(j.at("memory").get<std::string>(), s.memory)) {
            return make_error(ErrorCode::InvalidArgument, "unknown memory profile");
        }
        if (j.contains("model_roots")) s.model_roots = j.at("model_roots").get<std::vector<std::string>>();
        if (j.contains("log_level")) s.log_level = j.at("log_level").get<std::string>();
        if (j.contains("context_tokens")) s.context_tokens = j.at("context_tokens").get<std::uint32_t>();
        if (auto v = validate(s); !v) return v.error();
        return s;
    } catch (const json::exception& e) {
        return make_error(ErrorCode::InvalidArgument, std::string("settings type error: ") + e.what());
    }
}

Result<Settings> load_settings(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return Settings{};
    auto text = read_file_limited(path, 1u << 20);
    if (!text) return text.error();
    return parse_settings(text.value());
}

Result<void> save_settings(const fs::path& path, const Settings& s) {
    if (auto v = validate(s); !v) return v;
    return atomic_write_file(path, settings_to_json(s));
}

}  // namespace hcs
