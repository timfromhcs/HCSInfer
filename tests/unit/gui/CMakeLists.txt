hcs_add_gtest(gui_assets
  SOURCES test_gui_assets.cpp
  LINK hcs_gui
  DEFINES "HCS_GUI_ASSET_DIR=\"${CMAKE_SOURCE_DIR}/src/gui/assets\""
  LABELS unit)
