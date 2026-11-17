hcs_add_gtest(agent
  SOURCES test_sandbox_policy.cpp test_agent_config.cpp
  LINK hcs_agent hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
