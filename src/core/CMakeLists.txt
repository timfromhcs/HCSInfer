hcs_add_library(core
  SOURCES
    logger.cpp lifecycle.cpp fs_util.cpp path_guard.cpp memory_budget.cpp
    settings.cpp job_queue.cpp model_registry.cpp gguf_probe.cpp benchmark.cpp state_dir.cpp
  PUBLIC_LINK nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(hcs_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(hcs_core PRIVATE psapi)  # process peak memory in the benchmark runner
endif()
