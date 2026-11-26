# Overload protection over a real HTTP server: 503/429 + Retry-After, kill switch, watchdog (plan F1).
hcs_add_gtest(api_overload
  SOURCES test_api_overload.cpp
  LINK hcs_api hcs_scheduler hcs_core nlohmann_json::nlohmann_json
  LABELS contract)
target_include_directories(hcs_test_api_overload SYSTEM PRIVATE "${HCS_HTTPLIB_INCLUDE_DIR}")
target_compile_definitions(hcs_test_api_overload PRIVATE _WIN32_WINNT=0x0A00)
if(MSVC)
  set_source_files_properties(test_api_overload.cpp PROPERTIES COMPILE_OPTIONS "/W3;/WX-")
endif()
if(WIN32)
  target_link_libraries(hcs_test_api_overload PRIVATE ws2_32)
endif()
