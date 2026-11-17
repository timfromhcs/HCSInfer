hcs_add_library(mcp
  SOURCES mcp_server.cpp stdio.cpp
  PUBLIC_LINK hcs_api hcs_core nlohmann_json::nlohmann_json)
