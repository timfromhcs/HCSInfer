# HCS Train (ADR-012): dataset validator, eval-report ingestion, eval gate, adapter book and pipeline.
# Pure logic and bounded file I/O: it never runs a trainer and never downloads anything.
hcs_add_library(train
  SOURCES
    dataset_validator.cpp eval_gate.cpp eval_report.cpp adapter_book.cpp pipeline.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK nlohmann_json::nlohmann_json)
