# hcs_hardware: runtime hardware/driver/storage detection ("doctor").
# Vulkan is loaded dynamically (vulkan-1.dll); only the pinned, header-only Vulkan-Headers are
# needed at build time (no Vulkan SDK, no import library, nothing at runtime).
include(FetchContent)
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        v1.4.357   # commit e3b1eec08173d6b825cd3ac88c885a63b621504a
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(vulkan_headers)

hcs_add_library(hardware
  SOURCES doctor.cpp cpu_info.cpp system_info.cpp gpu_info.cpp vulkan_info.cpp storage_info.cpp
          report_format.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK nlohmann_json::nlohmann_json Vulkan::Headers)
target_compile_definitions(hcs_hardware PRIVATE VK_NO_PROTOTYPES)
target_include_directories(hcs_hardware PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
  target_link_libraries(hcs_hardware PRIVATE advapi32)
endif()
