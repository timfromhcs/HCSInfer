# CPU oracle + multi-threaded baseline: hand-computed values, properties, and baseline-vs-oracle agreement.
hcs_add_gtest(kernels_oracle
  SOURCES test_oracle.cpp
  LINK hcs_kernels_cpu hcs_core
  LABELS "kernel-cpu")
