# Pinned JSON library (header-only): exact release tag (+ commit recorded for provenance).
include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0   # commit 55f93686c01528224f448c19128836e7df245f72
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)
