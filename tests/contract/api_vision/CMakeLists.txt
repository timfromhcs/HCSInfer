hcs_add_gtest(api_vision_contract
  SOURCES test_vision_contract.cpp
  LINK hcs_mcp hcs_api hcs_core nlohmann_json::nlohmann_json
  LABELS contract)
