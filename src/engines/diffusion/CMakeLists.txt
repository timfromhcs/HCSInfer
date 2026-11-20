# stable-diffusion.cpp image engine adapter (hcs_engine_diffusion). The upstream tree is built from
# external/stable-diffusion.cpp (pinned) as one static library; stable-diffusion.h and stb headers
# never leave this module.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/stable-diffusion.cpp/CMakeLists.txt")
  message(STATUS "hcs_engine_diffusion: external/stable-diffusion.cpp missing - module skipped")
  return()
endif()

if(NOT TARGET stable-diffusion)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SD_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SD_BUILD_SHARED_GGML_LIB OFF CACHE BOOL "" FORCE)
  set(SD_SERVER_BUILD_FRONTEND OFF CACHE BOOL "" FORCE)
  # libwebp/libwebm are only linked by stable-diffusion.cpp's own CLI/server examples (examples/*/CMakeLists.txt),
  # which HCSInfer never builds. Pin both OFF so the result does not depend on whether the nested submodules are
  # checked out or on a stale cached default (the SBOM reads these cache values to report what is linked).
  set(SD_WEBP OFF CACHE BOOL "" FORCE)
  set(SD_WEBM OFF CACHE BOOL "" FORCE)
  if(HCS_ENABLE_VULKAN)
    set(SD_VULKAN ON CACHE BOOL "" FORCE)
  else()
    set(SD_VULKAN OFF CACHE BOOL "" FORCE)
  endif()
  # M1 proved SD_USE_UPSTREAM_GGML=ON against llama.cpp's GGML (SD1.5 Q8_0 output byte-identical).
  # Reusing the single ggml target avoids two incompatible GGML copies in one process (AGENTS.md 3.3).
  if(TARGET ggml AND EXISTS "${CMAKE_SOURCE_DIR}/external/llama.cpp/ggml/CMakeLists.txt")
    set(SD_USE_UPSTREAM_GGML ON CACHE BOOL "" FORCE)
    set(SD_GGML_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/llama.cpp/ggml" CACHE PATH "" FORCE)
  else()
    message(STATUS "hcs_engine_diffusion: no llama.cpp GGML target - using stable-diffusion.cpp's patched GGML")
    set(SD_USE_UPSTREAM_GGML OFF CACHE BOOL "" FORCE)
  endif()
  # Both upstream trees target C++17; HCS-owned code stays C++20 (AGENTS.md 3.4).
  set(_hcs_saved_cxx_std "${CMAKE_CXX_STANDARD}")
  set(CMAKE_CXX_STANDARD 17)
  add_subdirectory("${CMAKE_SOURCE_DIR}/external/stable-diffusion.cpp"
                   "${CMAKE_BINARY_DIR}/external/stable-diffusion.cpp" EXCLUDE_FROM_ALL)
  set(CMAKE_CXX_STANDARD "${_hcs_saved_cxx_std}")
endif()

hcs_add_library(engine_diffusion
  SOURCES diffusion_engine.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK stable-diffusion nlohmann_json::nlohmann_json)
# stb (public domain / MIT) and the upstream C API are third-party headers: no HCS warning policy.
target_include_directories(hcs_engine_diffusion SYSTEM PRIVATE
  "${CMAKE_SOURCE_DIR}/external/stable-diffusion.cpp/include"
  "${CMAKE_SOURCE_DIR}/external/stable-diffusion.cpp/thirdparty")
if(HCS_ENABLE_VULKAN)
  target_compile_definitions(hcs_engine_diffusion PRIVATE HCS_DIFFUSION_VULKAN=1)
endif()
