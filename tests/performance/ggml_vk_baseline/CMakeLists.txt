# Real-device tests of the in-process upstream GGML Vulkan baseline (wave C / k1): every operation the tune
# command compares against must run, pass its own double-precision spot check and report sane timing.
# Built only when the ggml Vulkan backend is part of the build; skips (NOT RUN) when no device is present.
if(NOT TARGET hcs_kernels_ggml_baseline)
  return()
endif()
hcs_add_gtest(ggml_vk_baseline
  SOURCES test_ggml_vk_baseline.cpp
  LINK hcs_kernels_ggml_baseline hcs_core
  LABELS "performance;kernel-vulkan")
