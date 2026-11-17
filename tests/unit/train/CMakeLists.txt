hcs_add_gtest(train
  SOURCES test_dataset_validator.cpp test_eval_gate.cpp
  LINK hcs_train hcs_core nlohmann_json::nlohmann_json
  LABELS unit)
