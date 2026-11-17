# HCS Train foundations (ADR-012): dataset validator and eval gate. Pure logic: no training, no downloads.
hcs_add_library(train
  SOURCES
    dataset_validator.cpp eval_gate.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK nlohmann_json::nlohmann_json)
