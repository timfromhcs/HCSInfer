# hcs_kernels_cpu: reference kernels (the correctness oracle and the dispatch fallback for the Vulkan
# kernels, AGENTS.md M8). Plain, dependency-free C++.
hcs_add_library(kernels_cpu
  SOURCES gemv_q8_0.cpp
  PUBLIC_LINK hcs_core)
