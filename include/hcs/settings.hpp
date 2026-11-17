#pragma once
// Versioned settings with atomic persistence and forward-migration.
//   v0 (legacy): {"host","port","model_dir"}  ->  v1: {"bind_host","port","model_roots":[...]}
// A file with schema_version > kSettingsSchemaVersion is rejected (never silently downgraded).
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/result.hpp"

namespace hcs {

inline constexpr int kSettingsSchemaVersion = 1;

enum class AccelProfile { Auto, Vulkan, CpuOnly };
enum class MemoryProfile { Auto, Fast, Balanced, LowMemory };

struct Settings {
    int schema_version = kSettingsSchemaVersion;
    std::string bind_host = "127.0.0.1";
    int port = 8765;
    bool allow_lan = false;  // explicit opt-in required for a non-loopback bind_host
    AccelProfile accel = AccelProfile::Auto;
    MemoryProfile memory = MemoryProfile::Auto;
    std::vector<std::string> model_roots;
    std::string log_level = "info";
    std::uint32_t context_tokens = 4096;
};

bool is_loopback_host(std::string_view host) noexcept;
std::string_view to_string(AccelProfile p) noexcept;
std::string_view to_string(MemoryProfile p) noexcept;

Result<void> validate(const Settings& s);
std::string settings_to_json(const Settings& s);
Result<Settings> parse_settings(std::string_view json_text);                       // migrates + validates
Result<Settings> load_settings(const std::filesystem::path& path);                 // missing file => defaults
Result<void> save_settings(const std::filesystem::path& path, const Settings& s);  // validate + atomic

}  // namespace hcs
