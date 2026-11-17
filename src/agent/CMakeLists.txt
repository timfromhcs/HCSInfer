# HCS coding-agent layer (ADR-011): tool-call sandbox policy and the sidecar provider document / launch plan.
# Pure logic, no process spawning.
hcs_add_library(agent
  SOURCES
    sandbox_policy.cpp agent_config.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK nlohmann_json::nlohmann_json)
