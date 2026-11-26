# Self-test of tools/stress/stress.py against a fake HTTP server (no HCSInfer binary, no model). Skipped when no
# Python interpreter is found (reported by CMake, not silently passed).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME stress_harness_selftest
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/stress/test_stress.py")
  set_tests_properties(stress_harness_selftest PROPERTIES
    LABELS "unit;tools" TIMEOUT 180 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
else()
  message(STATUS "stress_harness_selftest: no Python interpreter found - test not registered")
endif()
