# Image LoRA library (manifest, scanner, compatibility rules). Engine-independent: no stable-diffusion.cpp types.
hcs_add_library(lora
  SOURCES lora_library.cpp
  PUBLIC_LINK hcs_core
  PRIVATE_LINK hcs_storage nlohmann_json::nlohmann_json)
