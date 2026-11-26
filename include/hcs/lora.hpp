#pragma once
// Image LoRA library: manifest format, scanner and compatibility rules.
//
// Folder layout (nothing is ever downloaded by HCSInfer; the user drops files in):
//   <approved root>/lora/image/manifest.json     {"schema_version":1,"loras":[ {entry}, ... ]}
//   <approved root>/lora/image/<file>.safetensors
// Entry: {"id","file","name","base_family","trigger_words":[...],"recommended_weight","licence","source","sha256"}.
// `file` is a bare file name inside the folder (no separators). A .safetensors file without an entry is listed
// as "unmanifested" and cannot be used: a LoRA without a declared base family would be a silent-garbage risk.
//
// Compatibility (AGENTS.md "no silent garbage"): a LoRA is only applied to the base-model family it was
// trained for. The manifest declares the family; the safetensors tensor names are an independent plausibility
// check (kohya/diffusers naming tells SD1.x, SDXL, FLUX and SD3 apart). Disagreement => the LoRA is "invalid".
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/engine.hpp"
#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"

namespace hcs {

inline constexpr std::size_t kMaxLorasPerRequest = 4;
inline constexpr float kMaxLoraWeight = 2.0f;  // |weight| bound; 0 is rejected (it would be a silent no-op)
inline constexpr std::uint64_t kMaxLoraFileBytes = 4ull << 30;

// Base-model families a manifest may declare.
bool is_known_lora_family(std::string_view family) noexcept;
// Architecture group of a family ("sd1" | "sdxl" | "flux" | "sd3"), "" for an unknown family.
std::string_view lora_family_group(std::string_view family) noexcept;

enum class LoraCompat { Compatible, Incompatible, Unknown };
// `model_family` "" means the loaded model's family is not recognised => Unknown (cannot be proven).
LoraCompat lora_compatibility(std::string_view lora_family, std::string_view model_family) noexcept;

// Plausibility guess from safetensors tensor names: "sd1" | "sdxl" | "flux" | "sd3" | "" (not conclusive).
std::string sniff_lora_group(const std::vector<std::string>& tensor_names);

// Weight must be finite, non-zero and |w| <= kMaxLoraWeight.
Result<void> validate_lora_weight(float weight);

struct LoraEntry {
    std::string id;
    std::string name;
    std::string file;                  // bare file name inside the library folder
    std::filesystem::path path;        // canonical path (inside an approved root); empty if unresolved
    std::string base_family;           // declared (manifest)
    std::string sniffed_group;         // from the tensor names ("" = inconclusive)
    std::vector<std::string> trigger_words;
    float recommended_weight = 1.0f;
    std::string licence;
    std::string source;
    std::string sha256;                // declared (lower-case hex) ("" = not declared)
    std::string sha256_status = "unverified";  // unverified | ok | mismatch
    std::uint64_t size_bytes = 0;
    std::uint64_t tensor_count = 0;
    std::string status = "ready";      // ready | invalid | unmanifested | missing
    std::vector<std::string> problems; // why it is not ready
    bool usable() const noexcept { return status == "ready"; }
};

struct LoraScanOptions {
    bool verify_hashes = false;  // compute SHA-256 of every file and compare with the manifest
};

class LoraLibrary {
public:
    // Scans `dir` (must resolve inside an approved root of `guard`). A missing folder is not an error: the
    // library is simply empty and `scan_note()` says so. A malformed manifest is a ModelError.
    Result<void> scan(const PathGuard& guard, const std::filesystem::path& dir, LoraScanOptions opt = {});

    const std::vector<LoraEntry>& entries() const noexcept { return entries_; }
    const LoraEntry* find(std::string_view id) const noexcept;
    const std::string& scan_note() const noexcept { return note_; }
    const std::filesystem::path& directory() const noexcept { return dir_; }

    // Resolves request-level references to engine specs. Errors are structured and fail closed:
    //   InvalidArgument  bad count/weight/duplicate id; incompatible base family ("lora_incompatible: ...")
    //   NotFound         unknown id
    //   ModelError       entry exists but is not ready (invalid / missing / hash mismatch)
    // `model_family` is the loaded/requested model's family ("" = unknown; then a LoRA is only accepted when
    // it is certain unless `allow_unknown_family`; the engine re-checks with the family it detects).
    struct Ref {
        std::string id;
        float weight = 1.0f;
        bool weight_given = false;  // false => the manifest's recommended weight
    };
    Result<std::vector<LoraSpec>> resolve(const std::vector<Ref>& refs, std::string_view model_family,
                                          bool allow_unknown_family = true) const;

private:
    std::vector<LoraEntry> entries_;
    std::string note_;
    std::filesystem::path dir_;
};

// Pure manifest parsing (no filesystem access) so the format is unit-testable.
struct ParsedLoraManifest {
    std::vector<LoraEntry> entries;
    std::vector<std::string> warnings;
};
Result<ParsedLoraManifest> parse_lora_manifest(std::string_view json_text);

}  // namespace hcs
