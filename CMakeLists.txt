cmake_minimum_required(VERSION 3.25)
# C is enabled because the pinned engines (ggml, stable-diffusion.cpp) contain C translation units and
# stable-diffusion.cpp exports the PUBLIC compile feature `c_std_11`, which CMake resolves in the scope of
# the consuming target. HCS-owned code stays C++20 (AGENTS.md 3.4); this does not change that.
project(HCSInfer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(HCS_BUILD_TESTS "Build HCSInfer tests" ON)
option(HCS_WARNINGS_AS_ERRORS "Treat warnings in HCS-owned targets as errors" ON)
option(HCS_ENABLE_VULKAN "Enable Vulkan-dependent HCS modules (modules must honour this)" OFF)
option(HCS_ENABLE_VULKAN_KERNELS "Build the experimental HCS Vulkan kernels (M8; needs the Vulkan SDK to build)" OFF)

# Provenance for benchmark JSON (AGENTS.md 21.1): commit at configure time, "+dirty" when the tree has changes.
set(HCS_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _hcs_head
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _hcs_head_rc)
  if(_hcs_head_rc EQUAL 0 AND _hcs_head)
    set(HCS_GIT_COMMIT "${_hcs_head}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _hcs_dirty
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_hcs_dirty)
      set(HCS_GIT_COMMIT "${HCS_GIT_COMMIT}+dirty")
    endif()
  endif()
endif()
if(CMAKE_BUILD_TYPE)
  set(HCS_BUILD_TYPE_LABEL "${CMAKE_BUILD_TYPE}")
else()
  set(HCS_BUILD_TYPE_LABEL "multi-config")
endif()

# Generated header (version) lives in the build tree.
configure_file("${CMAKE_SOURCE_DIR}/cmake/version.hpp.in"
               "${CMAKE_BINARY_DIR}/generated/hcs/version.hpp" @ONLY)

include(HcsModule)
include(HcsJson)
if(HCS_BUILD_TESTS)
  enable_testing()
  include(HcsDependencies)
endif()

# ---------------------------------------------------------------------------
# Module discovery. Each module owns ONE CMakeLists.txt under src/<module>/.
# A module that is already in this list needs NO edit here: create its
# CMakeLists.txt and it is picked up. A module may test `if(TARGET hcs_x)` for
# modules listed before it. Adding a brand-new module name = one line below.
# ---------------------------------------------------------------------------
set(HCS_MODULE_DIRS
  core hardware storage memory scheduler telemetry process agent train
  engines/llama engines/diffusion
  kernels/cpu kernels/vulkan
  gui api mcp
  cli app)
set(HCS_EXCLUDE_MODULES "" CACHE STRING "Semicolon list of src/<module> dirs to skip (isolated builds)")
foreach(_m IN LISTS HCS_MODULE_DIRS)
  if(_m IN_LIST HCS_EXCLUDE_MODULES)
    continue()
  endif()
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/${_m}/CMakeLists.txt")
    add_subdirectory("src/${_m}")
  endif()
endforeach()

# Tests: every tests/<layer>/<name>/CMakeLists.txt is picked up automatically.
if(HCS_BUILD_TESTS)
  foreach(_layer unit contract parser-fuzz kernels integration e2e performance soak)
    file(GLOB _test_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
         "${CMAKE_SOURCE_DIR}/tests/${_layer}/*")
    foreach(_d IN LISTS _test_dirs)
      if(IS_DIRECTORY "${_d}" AND EXISTS "${_d}/CMakeLists.txt")
        get_filename_component(_n "${_d}" NAME)
        if(_n IN_LIST HCS_EXCLUDE_MODULES)
          continue()
        endif()
        add_subdirectory("${_d}" "tests/${_layer}/${_n}")
      endif()
    endforeach()
  endforeach()
endif()
