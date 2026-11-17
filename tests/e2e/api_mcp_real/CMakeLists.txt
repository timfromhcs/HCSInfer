# Real-engine API/MCP end-to-end test. Needs HCS_TEST_GGUF and Python; reports SKIPPED (exit 77)
# - never a fake pass - when the model is missing. Only registered when the llama engine is built.
if(TARGET hcs_engine_llama)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME e2e_api_mcp_real
      COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/e2e_real.py"
              --exe "$<TARGET_FILE:hcsinfer>"
              --evidence "${CMAKE_SOURCE_DIR}/artifacts/e2e-api-mcp")
    set_tests_properties(e2e_api_mcp_real PROPERTIES LABELS "e2e" SKIP_RETURN_CODE 77 TIMEOUT 1800)
  endif()
endif()
