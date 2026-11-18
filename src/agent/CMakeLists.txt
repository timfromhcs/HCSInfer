# HCS coding-agent layer (ADR-011): tool-call sandbox policy, sidecar provider document / launch plan,
# secret redaction, agent event parser, tool-call audit tripwire and the session manager. Processes are
# started only through hcs::process (the single Job-Object launcher); this library never spawns anything itself.
hcs_add_library(agent
  SOURCES
    sandbox_policy.cpp agent_config.cpp agent_redact.cpp event_parser.cpp tool_audit.cpp session_manager.cpp
  PUBLIC_LINK hcs_core hcs_process nlohmann_json::nlohmann_json)
