include(HcsHttplib)

hcs_add_library(api
  SOURCES control_plane.cpp http_api.cpp json_schema.cpp runtime.cpp http_server.cpp control_plane_vision.cpp control_plane_image.cpp control_plane_benchmark.cpp image_probe.cpp data_uri.cpp chat_tools.cpp
  PUBLIC_LINK hcs_core nlohmann_json::nlohmann_json)

# cpp-httplib is third-party code confined to http_server.cpp: relaxed warnings for that file only.
target_include_directories(hcs_api SYSTEM PRIVATE "${HCS_HTTPLIB_INCLUDE_DIR}")
if(MSVC)
  set_source_files_properties(http_server.cpp PROPERTIES COMPILE_OPTIONS "/W3;/WX-")
else()
  set_source_files_properties(http_server.cpp PROPERTIES COMPILE_OPTIONS "-Wno-error")
endif()
set_source_files_properties(http_server.cpp PROPERTIES COMPILE_DEFINITIONS "_WIN32_WINNT=0x0A00")
if(WIN32)
  target_link_libraries(hcs_api PRIVATE ws2_32)
endif()

# Embedded web UI (ADR-009): `gui` is listed before `api` in HCS_MODULE_DIRS.
if(TARGET hcs_gui)
  target_link_libraries(hcs_api PRIVATE hcs_gui)
  target_compile_definitions(hcs_api PRIVATE HCS_HAVE_GUI=1)
endif()

if(TARGET hcs_hardware)
  target_link_libraries(hcs_api PRIVATE hcs_hardware)
  target_compile_definitions(hcs_api PRIVATE HCS_HAVE_HARDWARE=1)
endif()

if(TARGET hcs_engine_llama)
  target_sources(hcs_api PRIVATE llama_stream_adapter.cpp)
  target_link_libraries(hcs_api PRIVATE hcs_engine_llama)
  target_compile_definitions(hcs_api PRIVATE HCS_HAVE_LLAMA_ENGINE=1)
endif()

# Image engine (stable-diffusion.cpp adapter): HttpApi/MCP image routes need an IImageStreamEngine.
if(TARGET hcs_engine_diffusion)
  target_sources(hcs_api PRIVATE diffusion_default_engine.cpp)
  target_link_libraries(hcs_api PRIVATE hcs_engine_diffusion)
  target_compile_definitions(hcs_api PRIVATE HCS_HAVE_DIFFUSION_ENGINE=1)
endif()
