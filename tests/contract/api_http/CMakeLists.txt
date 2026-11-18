# Real-socket contract tests: HttpServer on an ephemeral loopback port + a real HTTP client
# (cpp-httplib). The test TU includes third-party headers, so it gets relaxed warnings.
hcs_add_gtest(api_http
  SOURCES test_http_socket.cpp test_http_transport_negative.cpp
  LINK hcs_api hcs_core nlohmann_json::nlohmann_json
  LABELS contract)
target_include_directories(hcs_test_api_http SYSTEM PRIVATE "${HCS_HTTPLIB_INCLUDE_DIR}")
target_compile_definitions(hcs_test_api_http PRIVATE _WIN32_WINNT=0x0A00)
if(MSVC)
  set_source_files_properties(test_http_socket.cpp test_http_transport_negative.cpp PROPERTIES COMPILE_OPTIONS "/W3;/WX-")
endif()
if(WIN32)
  target_link_libraries(hcs_test_api_http PRIVATE ws2_32)
endif()
