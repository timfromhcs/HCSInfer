#pragma once
// Keyed kernel profile cache + the promotion rule (AGENTS.md 12.5).
//
// Key: (device_key = vendor:device:driver:api, kernel_id, kernel_hash = FNV-1a/64 of the embedded SPIR-V,
// shape_bucket). Only entries that PASSED correctness are ever persisted ("never write persistent profiles
// until correctness passes"); entries for another device, driver, shader hash or bucket are never matched,
// so a driver update or a shader edit invalidates the cache by construction. Stored as JSON under the HCS
// state dir (HCS_DATA_DIR), written atomically.
//
// Promotion rule (hcsuda README "rules", encoded here): correct vs the CPU oracle AND >= 3 bit-identical
// runs AND >= 1.05x vs the baseline AND not noisy. "Default-enabled" additionally needs a measured upstream
// GGML Vulkan baseline of >= 1.05x.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "hcs/result.hpp"

namespace hcs::hardware {

inline constexpr int kProfileSchemaVersion = 1;

struct ProfileEntry {
    // Key.
    std::string device_key;
    std::string kernel_id;
    std::string kernel_hash;
    std::string shape_bucket;

    // Context (informational).
    std::string device_name;
    std::string driver_info;
    std::uint64_t m = 0, n = 0, k = 0;  // exact benchmarked shape
    std::string timestamp_utc;
    std::string hcs_commit;

    // Correctness vs the CPU oracle.
    bool correct = false;
    double max_abs_error = 0.0;
    double tol_rel = 0.0, tol_abs = 0.0;
    std::uint32_t identical_runs = 0;  // runs whose output was bit-identical to the first
    std::uint32_t total_runs = 0;

    // Timing. `wall_*` = one dispatch incl. submit + fence with data resident (what a caller experiences);
    // `gpu_*` = timestamp queries (nullopt when the queue has no valid timestamp bits).
    std::uint32_t samples = 0;
    double wall_ms_median = 0.0, wall_spread = 0.0;  // spread = (p90 - p10) / median
    std::optional<double> gpu_ms_median;
    std::optional<double> gpu_spread;
    double flops = 0.0;  // theoretical work of one dispatch (0 = unknown)

    // Baseline the decision is made against (name + median wall ms of the CPU baseline implementation).
    std::string baseline_name;
    double baseline_ms = 0.0;
    // Upstream GGML Vulkan op baseline: "not_run" or "measured" (then ratio = upstream_ms / wall_ms_median).
    std::string upstream_status = "not_run";
    std::optional<double> upstream_ratio;

    double speedup() const;  // baseline_ms / wall_ms_median, 0 when either is not positive
    std::optional<double> gflops_wall() const;
    std::optional<double> gflops_gpu() const;
};

struct PromotionPolicy {
    std::uint32_t min_identical_runs = 3;
    double min_speedup = 1.05;
    double max_spread = 0.25;  // noisier than this is "not a result"
};

struct PromotionDecision {
    bool correct_ok = false;
    bool determinism_ok = false;
    bool noise_ok = false;
    bool speed_ok = false;
    bool promoted = false;        // all four
    bool default_enabled = false;  // promoted + measured upstream baseline >= min_speedup
    std::vector<std::string> reasons;  // why not (empty when default_enabled)
};

PromotionDecision evaluate_promotion(const ProfileEntry& e, const PromotionPolicy& policy = {});

class KernelProfile {
public:
    // Inserts or replaces the entry with the same key. Entries that did not pass correctness are rejected
    // (returns InvalidArgument) so a failing kernel can never be persisted.
    Result<void> upsert(ProfileEntry e);
    // Exact-key lookup (device_key, kernel_id, kernel_hash, shape_bucket).
    const ProfileEntry* find(const std::string& device_key, const std::string& kernel_id,
                             const std::string& kernel_hash, const std::string& bucket) const;
    // True when an entry exists for the (device, kernel, bucket) but under a different driver/hash: stale.
    bool has_stale(const std::string& device_key, const std::string& kernel_id, const std::string& kernel_hash,
                   const std::string& bucket) const;
    // Drops every entry of `device_key` (all kernels), or only `kernel_id` when non-empty. Returns the count.
    std::size_t reset(const std::string& device_key_prefix, const std::string& kernel_id = {});
    // Drops entries whose (device_key, kernel hash) do not match the given live pair. Returns the count.
    std::size_t prune_stale(const std::string& live_device_key,
                            const std::vector<std::pair<std::string, std::string>>& live_kernel_hashes);

    const std::vector<ProfileEntry>& entries() const { return entries_; }

    std::string to_json_text() const;
    static Result<KernelProfile> from_json_text(const std::string& text);

private:
    std::vector<ProfileEntry> entries_;
};

// Default file: <state_dir>/kernel-profile.json.
std::filesystem::path profile_path(const std::filesystem::path& state_dir);
// A missing file is an empty profile (not an error). A corrupt / wrong-schema file is an error and is never
// silently replaced by the caller.
Result<KernelProfile> load_profile(const std::filesystem::path& path);
Result<void> save_profile(const std::filesystem::path& path, const KernelProfile& profile);

// FNV-1a 64-bit over raw bytes, lowercase hex (16 chars). Used for the kernel hash.
std::string fnv1a64_hex(const void* data, std::size_t size);

}  // namespace hcs::hardware
