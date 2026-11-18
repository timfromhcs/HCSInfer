hcs_add_gtest(api
  SOURCES test_http_handler.cpp test_http_benchmarks.cpp
  LINK hcs_api hcs_core nlohmann_json::nlohmann_json
  LABELS contract)
