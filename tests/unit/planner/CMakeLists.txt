# Caps decoding, conservative planner, promotion rule and kernel profile cache (pure logic, synthetic data;
# ported from the hcsuda planner/caps unit tests, see docs/hcsuda-port.md).
hcs_add_gtest(planner
  SOURCES test_caps.cpp test_planner.cpp test_kernel_profile.cpp
  LINK hcs_hardware hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
target_include_directories(hcs_test_planner PRIVATE "${CMAKE_SOURCE_DIR}/src/hardware")
