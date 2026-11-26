#pragma once
// Injectable resource probe for the overload-controller tests (fault injection; not hardware evidence).
#include <atomic>
#include <mutex>

#include "hcs/telemetry/resource_probe.hpp"

namespace hcs::test {

constexpr std::uint64_t kMiB = 1ull << 20;
constexpr std::uint64_t kGiB = 1ull << 30;

class FakeProbe {
public:
    FakeProbe() { set_free(16 * kGiB); }
    void set_free(std::uint64_t bytes) {
        std::lock_guard lk(m_);
        s_.free_physical_bytes = bytes;
        s_.total_physical_bytes = 21 * kGiB;
    }
    void set_commit(std::uint64_t used, std::uint64_t limit) {
        std::lock_guard lk(m_);
        s_.commit_used_bytes = used;
        s_.commit_limit_bytes = limit;
    }
    void set_gpu(std::optional<std::uint64_t> headroom) {
        std::lock_guard lk(m_);
        s_.gpu_heap_headroom_bytes = headroom;
    }
    void make_unknown() {
        std::lock_guard lk(m_);
        s_ = telemetry::ResourceSample{};
    }
    telemetry::ResourceSample get() {
        ++calls;
        std::lock_guard lk(m_);
        return s_;
    }
    telemetry::ResourceProbe probe() {
        return [this] { return get(); };
    }
    std::atomic<int> calls{0};

private:
    std::mutex m_;
    telemetry::ResourceSample s_;
};

}  // namespace hcs::test
