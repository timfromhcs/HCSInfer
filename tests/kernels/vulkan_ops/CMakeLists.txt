# Real-device differential tests of the HCS Vulkan kernels vs the CPU oracle (label kernel-vulkan).
# Built only with -DHCS_ENABLE_VULKAN_KERNELS=ON; skips cleanly (NOT RUN) when no Vulkan device is present.
if(NOT TARGET hcs_kernels_vulkan)
  return()
endif()
hcs_add_gtest(vulkan_ops
  SOURCES test_vulkan_ops.cpp
  LINK hcs_kernels_vulkan hcs_kernels_cpu hcs_hardware hcs_core
  LABELS "kernel-vulkan")
