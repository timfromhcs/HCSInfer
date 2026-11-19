hcs_add_gtest(kernels
  SOURCES test_gemv_q8_0.cpp
  LINK hcs_kernels_cpu hcs_core
  LABELS "kernel-cpu")
