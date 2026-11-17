hcs_add_gtest(storage_mutation_fuzz
  SOURCES test_mutation_fuzz.cpp
  LINK hcs_storage hcs_core
  LABELS parser)
target_include_directories(hcs_test_storage_mutation_fuzz PRIVATE
  "${CMAKE_SOURCE_DIR}/tests/unit/storage" "${CMAKE_CURRENT_SOURCE_DIR}")

# Coverage-guided libFuzzer target: opt-in, Clang only (MSVC cl has no libFuzzer runtime).
option(HCS_BUILD_FUZZERS "Build libFuzzer harnesses (Clang with -fsanitize=fuzzer)" OFF)
if(HCS_BUILD_FUZZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(hcsmodel_fuzz hcsmodel_fuzz.cpp)
  target_link_libraries(hcsmodel_fuzz PRIVATE hcs_storage)
  target_include_directories(hcsmodel_fuzz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(hcsmodel_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
  target_link_options(hcsmodel_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
