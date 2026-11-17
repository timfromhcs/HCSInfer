# llama.cpp text engine adapter (hcs_engine_llama). Upstream is built from external/llama.cpp
# (pinned) as static libraries; llama.h / ggml.h never leave this module.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/llama.cpp/CMakeLists.txt")
  message(STATUS "hcs_engine_llama: external/llama.cpp missing - module skipped")
  return()
endif()

if(NOT TARGET llama)
  # One GGML struct layout for the whole process: stable-diffusion.cpp is built in
  # SD_USE_UPSTREAM_GGML mode against this GGML tree, and its src/core/ggml_extend.h asserts
  # GGML_MAX_NAME >= 160. If only sd.cpp's TUs saw 160 while ggml/llama used the default 64,
  # sizeof(ggml_tensor) would differ across the ABI boundary and corrupt every field access
  # (observed as a ggml_view_* bounds assertion while loading a real SD1.5 Q8_0 model). The value
  # is applied to the ggml/llama sources and to sd.cpp (which defines the same 160 itself).
  add_compile_definitions(GGML_MAX_NAME=160)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)
  # libmtmd (multimodal: image encoder + mmproj projector). Upstream's standalone hook builds only
  # tools/mtmd. Video input needs a subprocess ffmpeg and is not part of V1 (images only).
  set(LLAMA_BUILD_MTMD     ON  CACHE BOOL "" FORCE)
  set(MTMD_VIDEO           OFF CACHE BOOL "" FORCE)
  set(LLAMA_OPENSSL        OFF CACHE BOOL "" FORCE)
  set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
  set(GGML_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
  set(GGML_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
  if(HCS_ENABLE_VULKAN)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
  else()
    set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
  endif()
  # Upstream is built for C++17 (u8 literals break under C++20 char8_t); HCS-owned code stays C++20
  # (AGENTS.md 3.4: never force the HCS standard on third-party targets).
  set(_hcs_saved_cxx_std "${CMAKE_CXX_STANDARD}")
  set(CMAKE_CXX_STANDARD 17)
  add_subdirectory("${CMAKE_SOURCE_DIR}/external/llama.cpp"
                   "${CMAKE_BINARY_DIR}/external/llama.cpp" EXCLUDE_FROM_ALL)
  set(CMAKE_CXX_STANDARD "${_hcs_saved_cxx_std}")
endif()

# libmtmd compiles its own copy of stb_image (vendor/stb, STB_IMAGE_IMPLEMENTATION in mtmd-helper.cpp) with
# external linkage, and hcs_engine_diffusion compiles stable-diffusion.cpp's copy: both would define the same
# public stbi_* symbols in the single HCSInfer process (LNK2005). Renaming mtmd's copy through the preprocessor
# keeps both libraries unmodified (no upstream edit) and gives each its own decoder.
if(TARGET mtmd)
  set(_hcs_stbi_symbols
    stbi_convert_iphone_png_to_rgb
    stbi_convert_iphone_png_to_rgb_thread
    stbi_convert_wchar_to_utf8
    stbi_failure_reason
    stbi_hdr_to_ldr_gamma
    stbi_hdr_to_ldr_scale
    stbi_image_free
    stbi_info
    stbi_info2
    stbi_info2_from_callbacks
    stbi_info2_from_file
    stbi_info2_from_memory
    stbi_info_from_callbacks
    stbi_info_from_file
    stbi_info_from_memory
    stbi_is_16_bit
    stbi_is_16_bit_from_callbacks
    stbi_is_16_bit_from_file
    stbi_is_16_bit_from_memory
    stbi_is_hdr
    stbi_is_hdr_from_callbacks
    stbi_is_hdr_from_file
    stbi_is_hdr_from_memory
    stbi_ldr_to_hdr_gamma
    stbi_ldr_to_hdr_scale
    stbi_load
    stbi_load_16
    stbi_load_16_from_callbacks
    stbi_load_16_from_memory
    stbi_load_from_callbacks
    stbi_load_from_file
    stbi_load_from_file_16
    stbi_load_from_memory
    stbi_load_gif_from_memory
    stbi_loadf
    stbi_loadf_from_callbacks
    stbi_loadf_from_file
    stbi_loadf_from_memory
    stbi_set_flip_vertically_on_load
    stbi_set_flip_vertically_on_load_thread
    stbi_set_unpremultiply_on_load
    stbi_set_unpremultiply_on_load_thread
    stbi_zlib_decode_buffer
    stbi_zlib_decode_malloc
    stbi_zlib_decode_malloc_guesssize
    stbi_zlib_decode_malloc_guesssize_headerflag
    stbi_zlib_decode_noheader_buffer
    stbi_zlib_decode_noheader_malloc)
  foreach(_s IN LISTS _hcs_stbi_symbols)
    target_compile_definitions(mtmd PRIVATE "${_s}=hcs_mtmd_${_s}")
  endforeach()
endif()

hcs_add_library(engine_llama
  SOURCES llama_engine.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK llama mtmd)
if(HCS_ENABLE_VULKAN)
  target_compile_definitions(hcs_engine_llama PRIVATE HCS_LLAMA_VULKAN=1)
endif()
