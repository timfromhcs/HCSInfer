hcs_add_gtest(agent
  SOURCES test_sandbox_policy.cpp test_agent_config.cpp test_agent_redact.cpp test_event_parser.cpp
          test_tool_audit.cpp test_session_manager.cpp
  LINK hcs_agent hcs_process hcs_core nlohmann_json::nlohmann_json
  LABELS unit
  DEFINES HCS_CHILD_PROBE="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# The session-manager tests drive the real helper executable built next to the process tests.
add_dependencies(hcs_test_agent hcs_child_probe)
