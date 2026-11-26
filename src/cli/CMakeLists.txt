hcs_add_library(cli
  SOURCES cli.cpp cache_cmd.cpp chat_cmd.cpp image_cmd.cpp vision_cmd.cpp benchmark_cmd.cpp train_cmd.cpp merge_cmd.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK hcs_merge hcs_lora hcs_upscale nlohmann_json::nlohmann_json)

# HCS Train (module `train` is listed before `cli` in HCS_MODULE_DIRS): dataset validation, eval gate and
# adapter bookkeeping behind `HCSInfer train`.
if(TARGET hcs_train)
  target_link_libraries(hcs_cli PRIVATE hcs_train)
  target_compile_definitions(hcs_cli PRIVATE HCS_HAVE_TRAIN=1)
endif()

# Optional module delegation (set when the module's CMakeLists was added earlier in the
# top-level HCS_MODULE_DIRS order). See docs/build.md "Module conventions".
if(TARGET hcs_hardware)
  target_link_libraries(hcs_cli PRIVATE hcs_hardware)
  target_compile_definitions(hcs_cli PRIVATE HCS_HAVE_HARDWARE=1)
endif()

if(TARGET hcs_storage)
  target_link_libraries(hcs_cli PRIVATE hcs_storage)
  target_compile_definitions(hcs_cli PRIVATE HCS_HAVE_STORAGE=1)
endif()
if(WIN32)
  target_link_libraries(hcs_cli PRIVATE psapi)
endif()

if(TARGET hcs_engine_llama)
  target_link_libraries(hcs_cli PRIVATE hcs_engine_llama)
  target_compile_definitions(hcs_cli PRIVATE HCS_HAVE_LLAMA=1)
endif()

if(TARGET hcs_engine_diffusion)
  target_link_libraries(hcs_cli PRIVATE hcs_engine_diffusion)
  target_compile_definitions(hcs_cli PRIVATE HCS_HAVE_DIFFUSION=1)
endif()

# M3: HTTP API + MCP stdio (modules api/mcp are discovered before cli).
if(TARGET hcs_api)
  target_link_libraries(hcs_cli PRIVATE hcs_api)
  target_compile_definitions(hcs_cli PRIVATE HCS_HAVE_API=1)
endif()
if(TARGET hcs_mcp)
  target_link_libraries(hcs_cli PRIVATE hcs_mcp)
  target_compile_definitions(hcs_cli PRIVATE HCS_HAVE_MCP=1)
endif()
