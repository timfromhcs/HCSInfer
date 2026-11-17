# HCS coding-agent policy layer (ADR-011): tool-call sandbox policy. Pure logic, no process spawning.
hcs_add_library(agent
  SOURCES
    sandbox_policy.cpp
  PUBLIC_LINK hcs_core)
