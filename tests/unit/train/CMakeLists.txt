hcs_add_gtest(train
  SOURCES test_dataset_validator.cpp test_eval_gate.cpp test_eval_report.cpp test_adapter_book.cpp
          test_pipeline.cpp test_chat_template.cpp test_dataset_prep.cpp test_rsi_loop.cpp
  LINK hcs_train hcs_storage hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
