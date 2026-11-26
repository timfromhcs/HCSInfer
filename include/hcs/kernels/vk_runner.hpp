#pragma once
// Standalone Vulkan compute runner for HCS-owned kernels (AGENTS.md 12 / ADR-003 / ADR-014).
//
// Own VkInstance + VkDevice + compute queue (it never shares objects with the GGML Vulkan backend), RAII for
// every Vulkan object, buffers and pipelines keep the device alive (shared ownership), every wait is bounded
// (no unbounded hang: a dispatch that does not finish within the timeout marks the runner lost), and the
// Khronos validation layer can be requested and its messages are counted (debug builds request it by
// default). vulkan-1.dll is loaded dynamically (VK_NO_PROTOTYPES): the binary has no Vulkan import library
// and the runner reports "unavailable" on machines without a Vulkan loader.
//
// Design follows hcsuda @94a3485 crates/hcsuda-vk (device.rs, buffer.rs, pipeline.rs, dispatch.rs) rebuilt in
// C++20, see docs/hcsuda-port.md.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "hcs/hardware/caps.hpp"
#include "hcs/result.hpp"

namespace hcs::kernels::vulkan {

struct RunnerOptions {
    std::uint32_t device_index = 0;  // index in the Vulkan enumeration (same order as `doctor`)
    // Request VK_LAYER_KHRONOS_validation + VK_EXT_debug_utils. Not available -> `validation().active ==
    // false` (reported, never silently treated as "0 messages").
    bool validation = false;
    // Upper bound of one submit+wait. The OS watchdog (TDR, ~2 s) resets a GPU that is busy longer; this only
    // guarantees the host never waits forever.
    double dispatch_timeout_s = 10.0;
    // Place buffers in DEVICE_LOCAL memory (staged uploads) when a type exists; otherwise host-visible.
    bool prefer_device_local = true;
};

struct ValidationReport {
    bool requested = false;
    bool active = false;                      // layer was found and enabled
    std::uint64_t messages = 0;               // WARNING + ERROR messages seen
    std::vector<std::string> first_messages;  // at most 8
    std::string note;
};

struct DispatchTiming {
    double wall_ms = 0.0;          // submit -> fence signalled, host clock
    std::optional<double> gpu_ms;  // timestamp queries; nullopt when the queue has none
};

enum class Fault { None = 0, DeviceLostOnSubmit = 1 };

class Runner;
class Buffer;
class Pipeline;

namespace detail {
struct Impl;
}

class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    std::size_t size() const noexcept { return size_; }
    bool valid() const noexcept { return impl_ != nullptr; }

private:
    friend class Runner;
    friend struct detail::Impl;
    std::shared_ptr<detail::Impl> impl_;
    std::uint64_t buffer_ = 0;  // VkBuffer
    std::uint64_t memory_ = 0;  // VkDeviceMemory
    void* mapped_ = nullptr;    // non-null for host-visible buffers
    std::size_t size_ = 0;
    std::uint64_t generation_ = 0;
    void release() noexcept;
};

class Pipeline {
public:
    Pipeline() = default;
    ~Pipeline();
    Pipeline(Pipeline&&) noexcept;
    Pipeline& operator=(Pipeline&&) noexcept;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    bool valid() const noexcept { return impl_ != nullptr; }

private:
    friend class Runner;
    friend struct detail::Impl;
    std::shared_ptr<detail::Impl> impl_;
    std::uint64_t module_ = 0, dsl_ = 0, layout_ = 0, pipeline_ = 0;  // Vk handles
    std::uint32_t num_buffers_ = 0, push_bytes_ = 0;
    std::uint64_t generation_ = 0;
    void release() noexcept;
};

class Runner {
public:
    // Creates instance + device. Errors: Unsupported (no Vulkan loader / no device / no compute queue),
    // InvalidArgument (device_index out of range), BackendError (driver failure).
    static Result<Runner> create(const RunnerOptions& options = {});

    Runner(Runner&&) noexcept;
    Runner& operator=(Runner&&) noexcept;
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    // Capability record of the device this runner drives (decoded by the hardware probe).
    const hardware::DeviceCaps& caps() const;
    bool device_local_buffers() const;  // buffers live in DEVICE_LOCAL memory (staged I/O)
    bool timestamps_available() const;
    ValidationReport validation() const;
    // True after a device-lost / timeout / injected fault until reset() succeeds. A lost runner refuses work.
    bool lost() const;

    Result<Buffer> create_buffer(std::size_t bytes);
    Result<void> upload(Buffer& dst, const void* src, std::size_t bytes);
    Result<void> download(const Buffer& src, void* dst, std::size_t bytes);

    // SPIR-V as embedded bytes (size multiple of 4, magic 0x07230203). `num_buffers` storage buffers at
    // bindings 0..n-1 and a push-constant range of `push_bytes` (<= device limit).
    Result<Pipeline> create_pipeline(const void* spirv, std::size_t spirv_bytes, std::uint32_t num_buffers,
                                     std::uint32_t push_bytes);

    // One submit, one bounded wait. `push` may be null when push_bytes == 0.
    Result<DispatchTiming> dispatch(const Pipeline& p, std::span<const Buffer* const> buffers,
                                    const void* push, std::uint32_t push_bytes, std::uint32_t gx,
                                    std::uint32_t gy, std::uint32_t gz);

    // Recovery path after lost(): tears the device down and re-creates it. Every Buffer/Pipeline created
    // before the reset becomes invalid (their use returns InvalidState; destroying them is safe).
    Result<void> reset();

    // Test seam (AGENTS.md 17.3 / ADR-003): the next dispatch behaves as if vkQueueSubmit returned
    // VK_ERROR_DEVICE_LOST. Consumed by that dispatch.
    void inject_fault_for_testing(Fault f);

private:
    Runner() = default;
    std::shared_ptr<detail::Impl> impl_;
    RunnerOptions options_;
};

}  // namespace hcs::kernels::vulkan
