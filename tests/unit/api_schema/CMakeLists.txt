hcs_add_gtest(api_schema
  SOURCES test_json_schema.cpp
  LINK hcs_api hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
