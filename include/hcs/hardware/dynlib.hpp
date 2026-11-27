#pragma once
// Minimal dynamic-library seam for the Vulkan loader (AGENTS.md 12 / ADR-003). The product never links the
// Vulkan import library: vulkan-1.dll (Windows), libvulkan.so.1 (Linux) or libvulkan.1.dylib (macOS, MoltenVK
// or the Vulkan SDK loader) is opened at run time, so a machine without a Vulkan driver still starts in CPU
// mode.
#include <string>

namespace hcs::hardware {

struct VulkanLoader {
    void* handle = nullptr;                  // library handle; nullptr when no loader could be opened
    void* get_instance_proc_addr = nullptr;  // address of vkGetInstanceProcAddr; nullptr when not exported
    std::string path;                        // resolved file the loader was read from ("" when unknown)
    std::string tried;                       // human-readable list of names that were attempted
    bool loaded() const noexcept { return handle != nullptr; }
};

// Opens the platform's Vulkan loader once per process and caches the result (thread-safe). The library is
// process-lifetime: driver threads may still be shutting down at exit, so it is never unloaded.
const VulkanLoader& vulkan_loader();

// Name of the platform loader for messages ("vulkan-1.dll", "libvulkan.so.1", "libvulkan.1.dylib").
const char* vulkan_loader_name() noexcept;

}  // namespace hcs::hardware
