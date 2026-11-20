# Shutdown while idle / loading / generating with the REAL llama.cpp engine behind the real HTTP server.
# Needs HCS_TEST_GGUF (a small GGUF); otherwise every test reports NOT RUN (skipped), never a fake pass.
# The fake-engine variants of the same checks live in tests/contract/api_http/test_http_shutdown.cpp.
if(TARGET hcs_engine_llama AND TARGET hcs_api)
  hcs_add_gtest(shutdown_real
    SOURCES test_shutdown_real.cpp
    LINK hcs_api hcs_engine_llama hcs_core nlohmann_json::nlohmann_json
    LABELS integration)
  target_include_directories(hcs_test_shutdown_real SYSTEM PRIVATE "${HCS_HTTPLIB_INCLUDE_DIR}")
  target_compile_definitions(hcs_test_shutdown_real PRIVATE _WIN32_WINNT=0x0A00)
  if(MSVC)
    set_source_files_properties(test_shutdown_real.cpp PROPERTIES COMPILE_OPTIONS "/W3;/WX-")
  endif()
  if(WIN32)
    target_link_libraries(hcs_test_shutdown_real PRIVATE ws2_32)
  endif()
endif()
