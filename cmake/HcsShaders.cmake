# HCS shader build: compile a GLSL compute shader to SPIR-V with the Vulkan SDK's glslc, validate it with
# spirv-val, and embed the bytes into a generated C++ header (AGENTS.md 12.3). The end user never needs the
# SDK: the SPIR-V is embedded in the binary.
#
# hcs_add_shader(<target> <symbol> <stage:compute> <source> [<target-env>])
#   Adds the generated header to <target>; building the target compiles + validates + embeds the shader.
#   <target-env> defaults to vulkan1.1; shaders that need GL_EXT_integer_dot_product / cooperative matrix
#   pass vulkan1.3. With HCS_BUILD_TESTS a CTest entry "spirv-val.<symbol>" (label kernel-vulkan)
#   re-validates the built module after the build (AGENTS.md 16.5).
function(hcs_add_shader target symbol stage source)
  set(_env vulkan1.1)
  if(ARGC GREATER 4)
    set(_env "${ARGV4}")
  endif()
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
    COMMAND "${HCS_GLSLC}" --target-env=${_env} "-fshader-stage=${stage}" "${source}" -o "${_spv}"
    COMMAND "${HCS_SPIRV_VAL}" --target-env ${_env} "${_spv}"
    DEPENDS "${source}"
    COMMENT "Compiling + validating shader ${symbol} (glslc ${stage}, spirv-val ${_env})"
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
  if(HCS_BUILD_TESTS)
    add_test(NAME spirv-val.${symbol} COMMAND "${HCS_SPIRV_VAL}" --target-env ${_env} "${_spv}")
    set_tests_properties(spirv-val.${symbol} PROPERTIES LABELS "kernel-vulkan")
  endif()
endfunction()
