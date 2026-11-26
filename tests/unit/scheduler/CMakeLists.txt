# Unit tests for the overload controller (hcs_scheduler) and the resource samples (hcs_telemetry).
hcs_add_gtest(scheduler
  SOURCES test_admission.cpp test_resource_probe.cpp
  LINK hcs_scheduler hcs_telemetry hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
