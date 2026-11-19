if(NOT TARGET hcs_kernels_vulkan)
  return()  # built only with -DHCS_ENABLE_VULKAN_KERNELS=ON (and the Vulkan SDK present)
endif()
hcs_add_gtest(kernels_vulkan
  SOURCES test_gemv_q8_0_vk.cpp
  LINK hcs_kernels_vulkan hcs_kernels_cpu hcs_core
  LABELS "kernel-vulkan")
