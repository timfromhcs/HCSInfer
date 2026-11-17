#include "hcs/memory_budget.hpp"

#include <algorithm>

#include "hcs/checked_math.hpp"

namespace hcs {

std::string_view to_string(MemoryCategory c) noexcept {
    switch (c) {
        case MemoryCategory::ModelWeights:
            return "model_weights";
        case MemoryCategory::KvCache:
            return "kv_cache";
        case MemoryCategory::Workspace:
            return "workspace";
        case MemoryCategory::VulkanBuffers:
            return "vulkan_buffers";
        case MemoryCategory::VisionBuffers:
            return "vision_buffers";
        case MemoryCategory::DiffusionComponents:
            return "diffusion_components";
        case MemoryCategory::Runtime:
            return "runtime";
        case MemoryCategory::Prefetch:
            return "prefetch";
    }
    return "unknown";
}

std::uint64_t compute_application_budget(const MemoryMeasurements& m) noexcept {
    std::uint64_t phys =
        saturating_sub(saturating_sub(m.available_physical_bytes, m.os_reserve_bytes), m.safety_margin_bytes);
    if (m.vulkan_headroom_bytes)
        phys = std::min(phys, *m.vulkan_headroom_bytes);
    return phys;
}

void MemoryBudget::update_budget(std::uint64_t total_bytes) {
    std::lock_guard lock(mutex_);
    total_ = total_bytes;
}
void MemoryBudget::update_budget(const MemoryMeasurements& m) {
    update_budget(compute_application_budget(m));
}

std::uint64_t MemoryBudget::total() const {
    std::lock_guard lock(mutex_);
    return total_;
}
std::uint64_t MemoryBudget::reserved() const {
    std::lock_guard lock(mutex_);
    return reserved_;
}
std::uint64_t MemoryBudget::remaining() const {
    std::lock_guard lock(mutex_);
    return saturating_sub(total_, reserved_);
}
bool MemoryBudget::over_committed() const {
    std::lock_guard lock(mutex_);
    return reserved_ > total_;
}
bool MemoryBudget::can_fit(std::uint64_t bytes) const {
    std::lock_guard lock(mutex_);
    auto sum = checked_add(reserved_, bytes);
    return sum && *sum <= total_;
}

Result<void> MemoryBudget::reserve(const std::string& key, MemoryCategory c, std::uint64_t bytes,
                                   MemoryPlacement p) {
    std::lock_guard lock(mutex_);
    if (key.empty())
        return make_error(ErrorCode::InvalidArgument, "empty allocation key");
    if (bytes == 0)
        return make_error(ErrorCode::InvalidArgument, "zero-byte reservation");
    if (entries_.count(key)) {
        return make_error(ErrorCode::AlreadyExists, "region already reserved (no double counting): " + key);
    }
    auto sum = checked_add(reserved_, bytes);
    if (!sum || *sum > total_) {
        return make_error(ErrorCode::OutOfMemory, "reservation of " + std::to_string(bytes) + " bytes for '" +
                                                      key + "' exceeds remaining " +
                                                      std::to_string(saturating_sub(total_, reserved_)) +
                                                      " bytes");
    }
    reserved_ = *sum;
    entries_.emplace(key, MemoryEntry{key, c, p, bytes});
    return {};
}

Result<void> MemoryBudget::resize(const std::string& key, std::uint64_t new_bytes) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
        return make_error(ErrorCode::NotFound, "unknown region: " + key);
    if (new_bytes == 0)
        return make_error(ErrorCode::InvalidArgument, "zero-byte resize; use release()");
    const std::uint64_t without = reserved_ - it->second.bytes;
    auto sum = checked_add(without, new_bytes);
    if (!sum || *sum > total_) {
        return make_error(ErrorCode::OutOfMemory, "resize of '" + key + "' exceeds budget");
    }
    reserved_ = *sum;
    it->second.bytes = new_bytes;
    return {};
}

Result<void> MemoryBudget::release(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
        return make_error(ErrorCode::NotFound, "unknown region: " + key);
    reserved_ -= it->second.bytes;
    entries_.erase(it);
    return {};
}

std::uint64_t MemoryBudget::reserved_for(MemoryCategory c) const {
    std::lock_guard lock(mutex_);
    std::uint64_t sum = 0;
    for (const auto& [k, e] : entries_) {
        if (e.category == c)
            sum += e.bytes;  // bounded by reserved_ <= total_ <= UINT64_MAX
    }
    return sum;
}

std::vector<MemoryEntry> MemoryBudget::entries() const {
    std::lock_guard lock(mutex_);
    std::vector<MemoryEntry> out;
    out.reserve(entries_.size());
    for (const auto& [k, e] : entries_) out.push_back(e);
    return out;
}

}  // namespace hcs
