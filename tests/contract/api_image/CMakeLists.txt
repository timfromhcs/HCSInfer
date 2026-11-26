set(_defs "")
if(TARGET hcs_gui)
  list(APPEND _defs HCS_TEST_GUI=1)
endif()
hcs_add_gtest(api_image
  SOURCES test_image_routes.cpp
  LINK hcs_api hcs_core nlohmann_json::nlohmann_json
  LABELS contract
  DEFINES ${_defs})

hcs_add_gtest(api_image_lora_upscale
  SOURCES test_image_lora_upscale.cpp
  LINK hcs_api hcs_core hcs_lora hcs_upscale nlohmann_json::nlohmann_json
  LABELS contract)
