# Real-engine image API/MCP end-to-end test (HTTP + MCP + UI hook). Needs HCS_TEST_DIFFUSION_MODEL and Python;
# reports SKIPPED (exit 77) - never a fake pass - when the model is missing. Registered when the diffusion engine is built.
if(TARGET hcs_engine_diffusion AND TARGET hcs_api)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME e2e_image_real
      COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/e2e_image.py"
              --exe "$<TARGET_FILE:hcsinfer>"
              --evidence "${CMAKE_SOURCE_DIR}/artifacts/e2e-image")
    set_tests_properties(e2e_image_real PROPERTIES LABELS "e2e" SKIP_RETURN_CODE 77 TIMEOUT 3600)
  endif()
endif()
