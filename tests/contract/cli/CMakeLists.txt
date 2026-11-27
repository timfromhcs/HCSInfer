set(_defs "")
if(TARGET hcs_hardware)
  list(APPEND _defs HCS_HAVE_HARDWARE=1)
endif()
hcs_add_gtest(cli
  SOURCES test_cli.cpp test_cli_cache.cpp test_cli_remote.cpp test_cli_merge.cpp test_cli_upscale.cpp
  LINK hcs_cli hcs_upscale hcs_api hcs_storage hcs_core nlohmann_json::nlohmann_json
  LABELS contract
  DEFINES ${_defs})
