# HCS shader build: compile a GLSL compute shader to SPIR-V with the Vulkan SDK's glslc, validate it with
# spirv-val, and embed the bytes into a generated C++ header (AGENTS.md 12.3). The end user never needs the
# SDK: the SPIR-V is embedded in the binary.
#
# hcs_add_shader(<target> <symbol> <stage:compute> <source>)
#   Adds the generated header to <target>; building the target compiles + validates + embeds the shader.
function(hcs_add_shader target symbol stage source)
  find_program(HCS_GLSLC NAMES glslc glslc.exe HINTS "$ENV{VULKAN_SDK}/Bin")
  find_program(HCS_SPIRV_VAL NAMES spirv-val spirv-val.exe HINTS "$ENV{VULKAN_SDK}/Bin")
  if(NOT HCS_GLSLC)
    message(FATAL_ERROR "glslc not found. Install the Vulkan SDK and set VULKAN_SDK (build-time only).")
  endif()
  if(NOT HCS_SPIRV_VAL)
    message(FATAL_ERROR "spirv-val not found. Install the Vulkan SDK and set VULKAN_SDK (build-time only).")
  endif()
  set(_spv "${CMAKE_BINARY_DIR}/shaders/${symbol}.spv")
  set(_hdr "${CMAKE_BINARY_DIR}/generated/hcs/shaders/${symbol}.hpp")
  add_custom_command(
    OUTPUT "${_spv}"
    COMMAND "${HCS_GLSLC}" --target-env=vulkan1.2 "-fshader-stage=${stage}" "${source}" -o "${_spv}"
    COMMAND "${HCS_SPIRV_VAL}" --target-env vulkan1.2 "${_spv}"
    DEPENDS "${source}"
    COMMENT "Compiling + validating shader ${symbol} (glslc ${stage}, spirv-val vulkan1.2)"
    VERBATIM)
  add_custom_command(
    OUTPUT "${_hdr}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${_spv}" "-DOUTPUT=${_hdr}" "-DSYM=${symbol}"
            -P "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS "${_spv}"
    COMMENT "Embedding SPIR-V ${symbol}"
    VERBATIM)
  set_source_files_properties("${_hdr}" PROPERTIES GENERATED TRUE)
  target_sources(${target} PRIVATE "${_hdr}")
endfunction()
