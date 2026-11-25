hcs_add_gtest(core
  SOURCES
    test_result.cpp test_checked_math.cpp test_lifecycle.cpp test_memory_budget.cpp
    test_path_guard.cpp test_job_queue.cpp test_settings.cpp test_model_registry.cpp
    test_logger.cpp test_engine_interface.cpp test_gguf_probe.cpp test_benchmark.cpp test_fs_util.cpp
    test_image_profiles.cpp
  LINK hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
