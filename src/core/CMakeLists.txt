hcs_add_library(core
  SOURCES
    logger.cpp lifecycle.cpp fs_util.cpp path_guard.cpp memory_budget.cpp
    settings.cpp job_queue.cpp model_registry.cpp gguf_probe.cpp
  PRIVATE_LINK nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(hcs_core PUBLIC Threads::Threads)
