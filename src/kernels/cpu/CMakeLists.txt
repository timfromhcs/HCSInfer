# hcs_kernels_cpu: reference kernels (the correctness oracle and the dispatch fallback for the Vulkan
# kernels, AGENTS.md M8). Plain, dependency-free C++. oracle.cpp = hcsuda-ported single-threaded oracle;
# cpu_baseline.cpp = the multi-threaded speed baseline the tuner compares against.
find_package(Threads REQUIRED)
hcs_add_library(kernels_cpu
  SOURCES gemv_q8_0.cpp oracle.cpp cpu_baseline.cpp
  PUBLIC_LINK hcs_core Threads::Threads)
