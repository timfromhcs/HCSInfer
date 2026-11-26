# Model merging (linear, SLERP, TIES, DARE, LoRA-into-base) on safetensors / GGUF F32-F16-BF16 tensors.
hcs_add_library(merge
  SOURCES math.cpp tensor_file.cpp merge.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK hcs_storage nlohmann_json::nlohmann_json)
target_include_directories(hcs_merge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
