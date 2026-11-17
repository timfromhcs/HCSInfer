# Shared helpers for HCS-owned targets. Third-party targets never get these flags.

add_library(hcs_warnings INTERFACE)
if(MSVC)
  target_compile_options(hcs_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  if(HCS_WARNINGS_AS_ERRORS)
    target_compile_options(hcs_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(hcs_warnings INTERFACE -Wall -Wextra -Wpedantic)
  if(HCS_WARNINGS_AS_ERRORS)
    target_compile_options(hcs_warnings INTERFACE -Werror)
  endif()
endif()
if(WIN32)
  target_compile_definitions(hcs_warnings INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# hcs_add_library(<name> SOURCES ... [PUBLIC_LINK ...] [PRIVATE_LINK ...])
#   Creates STATIC library target hcs_<name> + alias hcs::<name>.
#   Public headers live in <repo>/include/hcs/ and are visible to every target.
function(hcs_add_library name)
  cmake_parse_arguments(A "" "" "SOURCES;PUBLIC_LINK;PRIVATE_LINK" ${ARGN})
  add_library(hcs_${name} STATIC ${A_SOURCES})
  add_library(hcs::${name} ALIAS hcs_${name})
  target_compile_features(hcs_${name} PUBLIC cxx_std_20)
  target_include_directories(hcs_${name} PUBLIC
    "${CMAKE_SOURCE_DIR}/include" "${CMAKE_BINARY_DIR}/generated")
  target_link_libraries(hcs_${name}
    PUBLIC ${A_PUBLIC_LINK}
    PRIVATE hcs_warnings ${A_PRIVATE_LINK})
endfunction()

# hcs_add_gtest(<name> SOURCES ... [LINK ...] [LABELS unit|contract|...] [DEFINES ...])
#   Creates executable hcs_test_<name>; every TEST is registered with CTest + labels.
function(hcs_add_gtest name)
  cmake_parse_arguments(A "" "" "SOURCES;LINK;LABELS;DEFINES" ${ARGN})
  if(NOT A_LABELS)
    set(A_LABELS unit)
  endif()
  add_executable(hcs_test_${name} ${A_SOURCES})
  target_link_libraries(hcs_test_${name} PRIVATE hcs_warnings ${A_LINK} GTest::gtest_main GTest::gmock)
  target_include_directories(hcs_test_${name} PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
  if(A_DEFINES)
    target_compile_definitions(hcs_test_${name} PRIVATE ${A_DEFINES})
  endif()
  include(GoogleTest)
  gtest_discover_tests(hcs_test_${name}
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "${A_LABELS}")
endfunction()
