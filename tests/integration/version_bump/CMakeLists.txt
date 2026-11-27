# scripts/bump-version.ps1 on a throw-away copy of the version-bearing files (never touches the source tree).
find_program(HCS_POWERSHELL_BUMP NAMES pwsh powershell)
if(HCS_POWERSHELL_BUMP)
  add_test(NAME version_bump_script
    COMMAND "${HCS_POWERSHELL_BUMP}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/test-bump-version.ps1" -Source "${CMAKE_SOURCE_DIR}")
  set_tests_properties(version_bump_script PROPERTIES LABELS "integration" TIMEOUT 300)
endif()
