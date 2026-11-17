# Held-out agentic eval (tests/eval/agentic): the scorer self-test runs everywhere; the real-model end-to-end run
# (chat, streaming, cancellation, tool calling, then the 40-task eval) needs HCS_TEST_LFM3 (LFM3-Omni-2B Q8_0 GGUF)
# and reports SKIPPED (exit 77, "NOT RUN") - never a fake pass - when it is missing.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(_eval_dir "${CMAKE_SOURCE_DIR}/tests/eval/agentic")
  add_test(NAME eval_agentic_scorer_selftest
    COMMAND "${Python3_EXECUTABLE}" -I "${_eval_dir}/score.py" --selftest)
  set_tests_properties(eval_agentic_scorer_selftest PROPERTIES LABELS "unit")
  if(TARGET hcs_engine_llama)
    add_test(NAME e2e_llama_agentic_real
      COMMAND "${Python3_EXECUTABLE}" -I "${_eval_dir}/e2e_lfm3.py"
              --exe "$<TARGET_FILE:hcsinfer>"
              --evidence "${CMAKE_SOURCE_DIR}/artifacts/e2e-lfm3")
    set_tests_properties(e2e_llama_agentic_real PROPERTIES LABELS "e2e" SKIP_RETURN_CODE 77 TIMEOUT 3600)
  endif()
endif()
