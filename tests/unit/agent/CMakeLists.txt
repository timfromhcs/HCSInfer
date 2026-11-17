hcs_add_gtest(agent
  SOURCES test_sandbox_policy.cpp
  LINK hcs_agent hcs_core
  LABELS unit)
