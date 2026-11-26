# hcs_scheduler: admission control, backpressure signals, kill switch and watchdog (plan F1).
hcs_add_library(scheduler
  SOURCES admission.cpp
  PUBLIC_LINK hcs_core hcs_telemetry nlohmann_json::nlohmann_json)
