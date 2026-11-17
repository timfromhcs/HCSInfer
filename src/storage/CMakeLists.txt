# HCS storage / Model Vault module: HCSMODEL container, codecs, bounded streaming, GGUF reader.
include(FetchContent)

# Pinned, statically linked codecs (exact release tags; commits recorded for provenance).
#   LZ4  v1.10.0  commit ebb370ca83af193212df4dcbadcc5d87bc0de2f0  (BSD-2-Clause for lib/)
#   zstd v1.5.7   commit f8745da6ff1ad1e7bab384bd1f9d742439278e99  (BSD-3-Clause option)
set(_hcs_saved_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lz4
  GIT_REPOSITORY https://github.com/lz4/lz4.git
  GIT_TAG        v1.10.0
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  build/cmake)
FetchContent_Declare(zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG        v1.5.7
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  build/cmake)
FetchContent_MakeAvailable(lz4 zstd)
set(BUILD_SHARED_LIBS "${_hcs_saved_shared}")

hcs_add_library(storage
  SOURCES
    sha256.cpp codec.cpp file_source.cpp hcsmodel.cpp stream.cpp gguf.cpp
    model_reader.cpp writer.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK lz4_static libzstd_static)
target_include_directories(hcs_storage PRIVATE "${lz4_SOURCE_DIR}/lib" "${zstd_SOURCE_DIR}/lib")

# Storage benchmark (AGENTS.md 11.5 / 21.1). Source lives in tools/benchmark-runner/.
add_executable(hcs_storage_bench "${CMAKE_SOURCE_DIR}/tools/benchmark-runner/storage_bench.cpp")
target_link_libraries(hcs_storage_bench PRIVATE hcs_storage hcs_warnings nlohmann_json::nlohmann_json)
target_compile_definitions(hcs_storage_bench PRIVATE HCS_CMAKE_VERSION="${CMAKE_VERSION}")
