# "Nothing on C:" guard: runs `HCSInfer doctor --json` between two snapshots of the C: cache directories and fails if
# they grew by more than 50 MB (scripts/check-no-c-writes.ps1). HCS_DATA_DIR points into this build tree so the
# run cannot touch %LOCALAPPDATA%\HCSInfer. Windows only; exit 77 (-> SKIP, "NOT RUN") when there is no drive C:.
if(WIN32 AND TARGET hcsinfer)
  find_program(HCS_POWERSHELL NAMES pwsh powershell)
  if(HCS_POWERSHELL)
    set(_guard_data "${CMAKE_BINARY_DIR}/guard-data")
    file(MAKE_DIRECTORY "${_guard_data}")
    add_test(NAME guard_no_c_writes_doctor
      COMMAND "${HCS_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
              -File "${CMAKE_SOURCE_DIR}/scripts/check-no-c-writes.ps1"
              -Exe "$<TARGET_FILE:hcsinfer>" -ArgString "doctor --json" -MaxGrowthMB 50
              -OutFile "${CMAKE_BINARY_DIR}/guard-no-c-writes.json")
    set_tests_properties(guard_no_c_writes_doctor PROPERTIES
      LABELS "guard" SKIP_RETURN_CODE 77 TIMEOUT 300
      ENVIRONMENT "HCS_DATA_DIR=${_guard_data}")
  endif()
endif()
