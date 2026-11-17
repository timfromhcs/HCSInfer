hcs_add_gtest(mcp_image
  SOURCES test_mcp_image.cpp
  LINK hcs_mcp hcs_api hcs_core nlohmann_json::nlohmann_json
  LABELS contract)
