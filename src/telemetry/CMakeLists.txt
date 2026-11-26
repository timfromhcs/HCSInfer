# hcs_telemetry: live resource samples (RAM, commit, working set, GPU heap headroom) for the overload controller.
hcs_add_library(telemetry
  SOURCES resource_probe.cpp
  PUBLIC_LINK hcs_core nlohmann_json::nlohmann_json)
if(TARGET hcs_hardware)
  target_link_libraries(hcs_telemetry PRIVATE hcs_hardware)
  target_compile_definitions(hcs_telemetry PRIVATE HCS_TELEMETRY_HAVE_HARDWARE=1)
endif()
