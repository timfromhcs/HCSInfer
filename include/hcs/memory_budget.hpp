#pragma once
// Global application memory budget (AGENTS.md 10.1). Pure arithmetic over supplied
// measurements - no OS queries here (hcs_hardware supplies live numbers).
//
// No double counting: every physical allocation is ONE ledger entry keyed by a unique
// region key. A UMA allocation visible to both CPU and Vulkan is reserved once
// (placement = SharedUma), never as RAM plus VRAM.
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/result.hpp"

namespace hcs {

enum class MemoryCategory {
    ModelWeights,
    KvCache,
    Workspace,
    VulkanBuffers,
    VisionBuffers,
    DiffusionComponents,
    Runtime,
    Prefetch,
};
enum class MemoryPlacement { Host, SharedUma, DeviceLocal };

std::string_view to_string(MemoryCategory c) noexcept;

struct MemoryMeasurements {
    std::uint64_t available_physical_bytes = 0;
    std::uint64_t os_reserve_bytes = 0;
    std::uint64_t safety_margin_bytes = 0;
    // Remaining Vulkan heap headroom (budget - usage) when VK_EXT_memory_budget is available.
    std::optional<std::uint64_t> vulkan_headroom_bytes;
};

// min(available - os_reserve - safety_margin, vulkan headroom); saturating, never negative.
std::uint64_t compute_application_budget(const MemoryMeasurements& m) noexcept;

struct MemoryEntry {
    std::string key;
    MemoryCategory category;
    MemoryPlacement placement;
    std::uint64_t bytes;
};

class MemoryBudget {
public:
    explicit MemoryBudget(std::uint64_t total_bytes = 0) : total_(total_bytes) {}
    explicit MemoryBudget(const MemoryMeasurements& m) : total_(compute_application_budget(m)) {}

    // Re-measure: the budget may shrink below what is reserved (over_committed()).
    void update_budget(std::uint64_t total_bytes);
    void update_budget(const MemoryMeasurements& m);

    std::uint64_t total() const;
    std::uint64_t reserved() const;
    std::uint64_t remaining() const;  // saturating
    bool over_committed() const;
    bool can_fit(std::uint64_t bytes) const;

    // InvalidArgument: empty key / zero bytes. AlreadyExists: key present (no double counting).
    // OutOfMemory: would exceed budget (including arithmetic overflow). State is unchanged on error.
    Result<void> reserve(const std::string& key, MemoryCategory c, std::uint64_t bytes,
                         MemoryPlacement p = MemoryPlacement::Host);
    Result<void> resize(const std::string& key, std::uint64_t new_bytes);
    Result<void> release(const std::string& key);  // NotFound if absent

    std::uint64_t reserved_for(MemoryCategory c) const;
    std::vector<MemoryEntry> entries() const;

    // Fault-injection seam at the allocator boundary (AGENTS.md 10.3 / 17.5; tests and `HCS_` diagnostics
    // only). Every reserve() and every growing resize() asks the hook first; when it returns true the call
    // fails with OutOfMemory exactly like a real refusal (ledger unchanged), without needing a huge model.
    // The hook runs under the budget mutex: it must not call back into this MemoryBudget. An empty
    // function (the default) disables injection.
    using FaultHook = std::function<bool(const std::string& key, MemoryCategory c, std::uint64_t bytes)>;
    void set_fault_injector(FaultHook hook);
    std::uint64_t injected_failures() const;  // refusals produced by the hook since construction

private:
    mutable std::mutex mutex_;
    std::uint64_t total_;
    std::uint64_t reserved_ = 0;
    std::map<std::string, MemoryEntry> entries_;
    FaultHook fault_hook_;
    std::uint64_t injected_failures_ = 0;
};

}  // namespace hcs
