hcs_add_gtest(storage_real_model
  SOURCES test_real_gguf.cpp
  LINK hcs_storage hcs_core
  LABELS integration)
