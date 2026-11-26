# 4K upscaler: tiled pipeline + Lanczos finish + a dynamically loaded ONNX Runtime backend.
# No compile-time ONNX Runtime dependency (the DLL is found and loaded at run time), so this module builds and
# the binary runs without ORT present.
hcs_add_library(upscale
  SOURCES tiling.cpp resample.cpp png_codec.cpp ort_engine.cpp
  PUBLIC_LINK hcs_core)

# PNG glue uses stb from the pinned stable-diffusion.cpp thirdparty tree (public domain / MIT). Without it the
# codec reports Unsupported and everything else still works.
set(_hcs_stb_dir "${CMAKE_SOURCE_DIR}/external/stable-diffusion.cpp/thirdparty")
if(EXISTS "${_hcs_stb_dir}/stb_image.h" AND EXISTS "${_hcs_stb_dir}/stb_image_write.h")
  target_include_directories(hcs_upscale SYSTEM PRIVATE "${_hcs_stb_dir}")
  target_compile_definitions(hcs_upscale PUBLIC HCS_UPSCALE_HAVE_STB=1)
else()
  message(STATUS "hcs_upscale: stb headers not found - PNG codec disabled")
endif()
