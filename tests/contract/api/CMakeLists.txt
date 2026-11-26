hcs_add_gtest(api
  SOURCES test_http_handler.cpp test_http_benchmarks.cpp test_runtime_lifecycle.cpp
          test_oom_injection.cpp test_vault_load.cpp
  LINK hcs_api hcs_storage hcs_core nlohmann_json::nlohmann_json
  LABELS contract)
