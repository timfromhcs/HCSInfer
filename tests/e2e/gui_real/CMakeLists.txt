# Web UI end-to-end test (see e2e_gui.py). Reports BLOCKED (exit 77 -> ctest SKIP, "NOT RUN") when the UI hook is not
# wired into the server yet or a prerequisite is missing - never a fake pass. Needs Python; a model via HCS_TEST_GGUF
# enables the load/chat steps, python `playwright` enables browser interaction.
if(TARGET hcsinfer)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME e2e_gui_real
      COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/e2e_gui.py"
              --exe "$<TARGET_FILE:hcsinfer>"
              --evidence "${CMAKE_SOURCE_DIR}/artifacts/e2e-gui")
    set_tests_properties(e2e_gui_real PROPERTIES LABELS "e2e" SKIP_RETURN_CODE 77 TIMEOUT 1800)
  endif()
endif()
