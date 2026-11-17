# Embedded local web UI (ADR-009). Assets in src/gui/assets are compiled into the binary as byte arrays by
# embed_assets.cmake at build time; the library has no third-party dependency.
set(HCS_GUI_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(HCS_GUI_ASSET_LIST index.html app.css app.js)
set(HCS_GUI_GENERATED "${CMAKE_BINARY_DIR}/generated/gui/gui_assets_data.cpp")

set(_deps "")
foreach(_a IN LISTS HCS_GUI_ASSET_LIST)
  list(APPEND _deps "${HCS_GUI_ASSET_DIR}/${_a}")
endforeach()

add_custom_command(
  OUTPUT "${HCS_GUI_GENERATED}"
  COMMAND ${CMAKE_COMMAND} -DASSET_DIR=${HCS_GUI_ASSET_DIR} -DOUT=${HCS_GUI_GENERATED}
          "-DASSETS=${HCS_GUI_ASSET_LIST}" -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.cmake"
  DEPENDS ${_deps} "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.cmake"
  COMMENT "Embedding GUI assets"
  VERBATIM)

hcs_add_library(gui SOURCES gui_assets.cpp "${HCS_GUI_GENERATED}")
set_source_files_properties("${HCS_GUI_GENERATED}" PROPERTIES GENERATED TRUE)
