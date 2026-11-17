# Real-VLM API/MCP end-to-end test (image input). Needs HCS_TEST_VLM (multimodal GGUF with its mmproj*.gguf in
# the same folder) and Python; reports SKIPPED (exit 77, "NOT RUN") - never a fake pass - when missing.
if(TARGET hcs_engine_llama)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME e2e_vision_real
      COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/e2e_vision.py"
              --exe "$<TARGET_FILE:hcsinfer>"
              --evidence "${CMAKE_SOURCE_DIR}/artifacts/e2e-vision")
    set_tests_properties(e2e_vision_real PROPERTIES LABELS "e2e" SKIP_RETURN_CODE 77 TIMEOUT 1800)
  endif()
endif()
