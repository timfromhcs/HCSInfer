#pragma once
// HCSInfer experimental Vulkan kernels (AGENTS.md M8 / docs/vulkan-kernels.md).
//
// The kernels are feature-flagged and default OFF: while disabled, gemv_q8_0() returns Unsupported so the
// caller keeps using the CPU reference (hcs/kernels/cpu.hpp). Nothing here is on the model load path yet;
// this is an isolated, correctness-first kernel with its own benchmark, not a claim about end-to-end speed.
#include <cstdint>
#include <string>
#include <string_view>

#include "hcs/result.hpp"

namespace hcs::kernels::vulkan {

struct DeviceInfo {
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t api_version = 0;  // raw VK_MAKE_VERSION value
    std::string device_type;        // integrated_gpu | discrete_gpu | ...
};

struct GemvRequest {
    const float* x = nullptr;      // k F32 values
    const void* w_q8_0 = nullptr;  // m rows x k cols of raw Q8_0 bytes (m * (k/32) * 34)
    float* y = nullptr;            // m F32 values (written)
    std::uint32_t m = 0;
    std::uint32_t k = 0;
};

// Kernel variants (AGENTS.md M8: keep the known-good implementation and feature-flag experiments).
//   Scalar : v1, one invocation per row, x read from global memory per row (simple). Default.
//   Tiled  : v2 experiment, x staged in workgroup shared memory per K-tile. On the Radeon 680M it measured
//            the same as Scalar (within noise) for the shapes tested, so it is NOT the default. Kept behind
//            this flag so the A/B stays reproducible (`hcs_kernel_bench`); see
//            docs/test-evidence/vulkan-kernels/.
enum class Variant { Scalar = 1, Tiled = 2 };
const char* to_string(Variant v) noexcept;

// Feature flag (AGENTS.md M8: "keep it behind a feature flag"). Default false.
bool enabled() noexcept;
void set_enabled(bool on) noexcept;

// True when a Vulkan compute device is reachable (independent of the feature flag).
bool available() noexcept;

// Describes the device the kernel would use. `device_name_substr` selects a device by a case-insensitive
// name substring (e.g. "Radeon"); empty selects the best default.
Result<DeviceInfo> device_info(std::string_view device_name_substr = {});

// y[m] = sum_k dequant_q8_0(W[m, k]) * x[k] on the GPU. Validates the shape and the feature flag first.
Result<void> gemv_q8_0(const GemvRequest& req, std::string_view device_name_substr = {},
                       Variant variant = Variant::Scalar);

// Test seam (not for production use): the next dispatch behaves as if vkQueueSubmit returned
// VK_ERROR_DEVICE_LOST. The kernel must return BackendError, reset its cached context and recover on the next
// call.
enum class TestFault { None = 0, DeviceLostOnSubmit = 1 };
void inject_fault_for_testing(TestFault f) noexcept;

// Releases the cached Vulkan device/pipeline. Optional; also runs at process exit.
void shutdown() noexcept;

}  // namespace hcs::kernels::vulkan
