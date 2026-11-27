// The Vulkan loader seam (src/hardware/dynlib.cpp). Machine-independent invariants only: whether a loader is
// installed depends on the host, so the "loaded" branch is checked when present and never required.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "hcs/hardware/dynlib.hpp"

using namespace hcs::hardware;

TEST(Dynlib, LoaderNameIsPlatformSpecificAndNonEmpty) {
    const std::string n = vulkan_loader_name();
#if defined(_WIN32)
    EXPECT_EQ(n, "vulkan-1.dll");
#elif defined(__APPLE__)
    EXPECT_EQ(n, "libvulkan.1.dylib");
#else
    EXPECT_EQ(n, "libvulkan.so.1");
#endif
}

TEST(Dynlib, LoaderIsCachedAndConsistent) {
    const VulkanLoader& a = vulkan_loader();
    const VulkanLoader& b = vulkan_loader();
    EXPECT_EQ(&a, &b);  // one process-wide instance, opened once
    EXPECT_FALSE(a.tried.empty());
    if (!a.loaded()) {
        EXPECT_EQ(a.get_instance_proc_addr, nullptr);
        GTEST_SKIP() << "NOT RUN (host): no Vulkan loader installed (tried: " << a.tried << ")";
    }
    EXPECT_NE(a.get_instance_proc_addr, nullptr);  // every conformant loader exports vkGetInstanceProcAddr
}
