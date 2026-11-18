# Real-model reliability soak (AGENTS.md 16.1G, M10). Every test needs HCS_SOAK_MODEL_ROOT (a folder holding
# LFM3-Omni-2B Q8_0 + mmproj and/or SD1.5 Q8_0) and reports NOT RUN (skip) without it - never a fake pass.
# Durations/iteration counts are env-driven: defaults are a quick smoke, scripts/soak.ps1 sets the long run.
#
# The executable is always built (scripts/soak.ps1 runs it directly) but its tests are registered with CTest ONLY
# when -DHCS_SOAK_REGISTER=ON, so the default `ctest` stays fast and never reports a soak as passed. Registered
# tests carry the label `soak`:  ctest -L soak  (needs HCS_SOAK_MODEL_ROOT, otherwise every test is skipped).
option(HCS_SOAK_REGISTER "Register the real-model soak gtests with CTest (label: soak)" OFF)
if(WIN32 AND TARGET hcs_api AND TARGET hcs_engine_llama AND TARGET hcs_storage)
  if(HCS_SOAK_REGISTER)
    hcs_add_gtest(soak_core
      SOURCES test_soak_core.cpp
      LINK hcs_api hcs_storage hcs_core nlohmann_json::nlohmann_json psapi
      LABELS soak)
  else()
    add_executable(hcs_test_soak_core test_soak_core.cpp)
    target_link_libraries(hcs_test_soak_core PRIVATE hcs_warnings hcs_api hcs_storage hcs_core
      nlohmann_json::nlohmann_json psapi GTest::gtest_main GTest::gmock)
    target_include_directories(hcs_test_soak_core PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
  endif()
endif()
