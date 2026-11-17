#pragma once
// Model registry + manifest. Discovery is read-only: scan() never writes, deletes or hashes
// (hashing is a later, explicit operation). File extensions are NOT trusted; headers are checked.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/lifecycle.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"

namespace hcs {

enum class ModelFormat { Unknown, Gguf, Safetensors, HcsModel };
enum class ModelKind { Unknown, Text, Vision, Image };

std::string_view to_string(ModelFormat f) noexcept;
std::string_view to_string(ModelKind k) noexcept;

struct ModelManifest {
    std::string id;  // ^[a-z0-9][a-z0-9._-]{0,63}$
    std::string display_name;
    std::string path;
    ModelFormat format = ModelFormat::Unknown;
    ModelKind kind = ModelKind::Unknown;
    std::uint64_t size_bytes = 0;
    std::string sha256;  // empty until hashed
    std::string architecture = "unknown";
    std::string quantization = "unknown";
    std::string license = "unknown";
};

bool is_valid_model_id(std::string_view id) noexcept;
// Derives a valid id from a file stem (lowercase, invalid chars -> '-').
std::string make_model_id(std::string_view stem);

struct ScanReport {
    std::size_t added = 0;
    std::size_t updated = 0;
    std::size_t skipped = 0;
    std::vector<std::string> messages;  // one line per skipped/rejected file
};

class ModelRegistry {
public:
    // Adds a model in state Discovered. AlreadyExists on duplicate id; InvalidArgument on bad id.
    Result<std::string> add(ModelManifest manifest);
    Result<ModelManifest> get(std::string_view id) const;
    std::vector<ModelManifest> list() const;  // sorted by id
    // Removing is refused while Validating/Loading/Loaded/Running/Unloading (InvalidState).
    Result<void> remove(std::string_view id);
    std::size_t size() const;

    Result<ModelState> state(std::string_view id) const;
    Result<void> transition(std::string_view id, ModelState to);
    Result<void> fail(std::string_view id, FailureKind kind, std::string reason);

    // Recursively scans `root` (depth <= 8, symlinks not followed). Every file is resolved
    // through `guard`; anything outside an approved root is skipped. Idempotent per path.
    Result<ScanReport> scan(const std::filesystem::path& root, const PathGuard& guard);

    std::string to_json() const;                    // {"schema_version":1,"models":[...]}
    Result<void> load_json(std::string_view text);  // adds models (state Discovered); skips existing ids

private:
    struct Entry {
        ModelManifest manifest;
        std::unique_ptr<ModelLifecycle> lifecycle;
    };
    Result<std::string> add_locked(ModelManifest manifest);
    mutable std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> models_;
};

}  // namespace hcs
