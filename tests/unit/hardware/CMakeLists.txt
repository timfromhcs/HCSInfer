# Unit tests (pure formatting/decoding, synthetic data) + live smoke tests of the doctor.
# Live tests that need real hardware report GTEST_SKIP with the reason (never a fake pass).
hcs_add_gtest(hardware
  SOURCES test_formatting.cpp test_cpu_decode.cpp test_doctor_live.cpp
  LINK hcs_hardware hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
target_include_directories(hcs_test_hardware PRIVATE "${CMAKE_SOURCE_DIR}/src/hardware")
