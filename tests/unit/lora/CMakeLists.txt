hcs_add_gtest(lora
  SOURCES test_lora_library.cpp
  LINK hcs_lora hcs_storage hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
