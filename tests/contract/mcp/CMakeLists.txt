hcs_add_gtest(mcp
  SOURCES test_mcp.cpp
  LINK hcs_mcp hcs_api hcs_core nlohmann_json::nlohmann_json
  LABELS contract)
