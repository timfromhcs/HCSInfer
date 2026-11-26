#include "hcs/telemetry/resource_probe.hpp"

#include <chrono>
#include <memory>
#include <mutex>

#ifdef HCS_TELEMETRY_HAVE_HARDWARE
#include "hcs/hardware/memory_probe.hpp"
#endif

namespace hcs::telemetry {

std::optional<double> ResourceSample::commit_fraction() const noexcept {
    if (!commit_used_bytes || !commit_limit_bytes || *commit_limit_bytes == 0)
        return std::nullopt;
    return static_cast<double>(*commit_used_bytes) / static_cast<double>(*commit_limit_bytes);
}

nlohmann::json to_json(const ResourceSample& s) {
    auto v = [](const std::optional<std::uint64_t>& o) {
        return o ? nlohmann::json(*o) : nlohmann::json("unknown");
    };
    nlohmann::json j = {{"total_physical_bytes", v(s.total_physical_bytes)},
                        {"free_physical_bytes", v(s.free_physical_bytes)},
                        {"commit_used_bytes", v(s.commit_used_bytes)},
                        {"commit_limit_bytes", v(s.commit_limit_bytes)},
                        {"process_working_set_bytes", v(s.process_working_set_bytes)},
                        {"gpu_heap_headroom_bytes", v(s.gpu_heap_headroom_bytes)}};
    const auto f = s.commit_fraction();
    j["commit_fraction"] = f ? nlohmann::json(*f) : nlohmann::json("unknown");
    return j;
}

ResourceProbe make_system_probe() {
#ifdef HCS_TELEMETRY_HAVE_HARDWARE
    struct GpuCache {
        std::mutex m;
        std::chrono::steady_clock::time_point at{};
        std::optional<std::uint64_t> value;
        bool valid = false;
    };
    auto cache = std::make_shared<GpuCache>();
    return [cache]() {
        ResourceSample s;
        const auto m = hardware::sample_system_memory();
        s.total_physical_bytes = m.total_physical_bytes;
        s.free_physical_bytes = m.available_physical_bytes;
        s.commit_used_bytes = m.commit_used_bytes;
        s.commit_limit_bytes = m.commit_limit_bytes;
        s.process_working_set_bytes = m.process_working_set_bytes;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lk(cache->m);
        if (!cache->valid || now - cache->at > std::chrono::seconds(1)) {
            cache->value = hardware::sample_gpu_heap_headroom();
            cache->at = now;
            cache->valid = true;
        }
        s.gpu_heap_headroom_bytes = cache->value;
        return s;
    };
#else
    return [] { return ResourceSample{}; };
#endif
}

}  // namespace hcs::telemetry
