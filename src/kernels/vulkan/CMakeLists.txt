# hcs_kernels_vulkan: experimental HCS Vulkan kernels (AGENTS.md M8). Feature-flagged and OFF by default;
# never on the model load path. Building it requires the Vulkan SDK (glslc + spirv-val) at build time only:
# the SPIR-V is embedded in the binary, so the end user does not need the SDK (see cmake/HcsShaders.cmake).
if(NOT HCS_ENABLE_VULKAN_KERNELS)
  return()
endif()
if(NOT TARGET Vulkan::Headers)
  message(FATAL_ERROR
    "HCS_ENABLE_VULKAN_KERNELS=ON needs the Vulkan::Headers target; it is created by the 'hardware' module, "
    "which must be built before 'kernels/vulkan'.")
endif()

include(HcsShaders)
hcs_add_library(kernels_vulkan
  SOURCES gemv_q8_0.cpp
  PUBLIC_LINK hcs_core hcs_kernels_cpu
  PRIVATE_LINK Vulkan::Headers)
# Dynamic loader: no Vulkan import library, nothing Vulkan at run time until the kernel is used.
target_compile_definitions(hcs_kernels_vulkan PRIVATE VK_NO_PROTOTYPES HCS_HAVE_VULKAN_KERNELS=1)
hcs_add_shader(hcs_kernels_vulkan GemvQ8_0 compute "${CMAKE_SOURCE_DIR}/shaders/llm/gemv_q8_0.comp")
hcs_add_shader(hcs_kernels_vulkan GemvQ8_0Tiled compute "${CMAKE_SOURCE_DIR}/shaders/llm/gemv_q8_0_tiled.comp")

# Kernel benchmark (AGENTS.md 12.5 / 16.6 B3): CPU reference vs the HCS Vulkan kernel, JSON out.
add_executable(hcs_kernel_bench "${CMAKE_SOURCE_DIR}/tools/kernel-bench/kernel_bench.cpp")
target_link_libraries(hcs_kernel_bench PRIVATE hcs_kernels_vulkan hcs_kernels_cpu hcs_warnings
                                             nlohmann_json::nlohmann_json)
target_include_directories(hcs_kernel_bench PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
