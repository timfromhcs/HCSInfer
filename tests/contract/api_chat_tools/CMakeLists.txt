hcs_add_gtest(api_chat_tools_contract
  SOURCES test_chat_tools_contract.cpp
  LINK hcs_api hcs_core nlohmann_json::nlohmann_json
  LABELS contract)
