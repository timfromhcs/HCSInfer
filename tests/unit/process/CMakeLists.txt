# hcs::process tests. The probe is a real helper executable (not a mock), spawned by the tests.
add_executable(hcs_child_probe child_probe.cpp)
target_link_libraries(hcs_child_probe PRIVATE hcs_warnings)

# The test locates the probe in the build's runtime output directory. The build path is embedded at configure
# time; a build directory containing spaces is not supported by this define (the repository default has none).
hcs_add_gtest(process
  SOURCES test_child_process.cpp test_child_process_stream.cpp
  LINK hcs_process hcs_core
  LABELS unit
  WINDOWS_ONLY   # Job Objects / kill-on-close: the POSIX sidecar launcher is Planned (module returns Unsupported)
  DEFINES HCS_CHILD_PROBE="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

if(TARGET hcs_test_process)
  add_dependencies(hcs_test_process hcs_child_probe)
endif()
