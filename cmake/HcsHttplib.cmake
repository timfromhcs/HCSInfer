# Pinned HTTP server library (header-only use): exact release tag + commit for provenance.
# We only download the sources (SOURCE_SUBDIR points to a non-existent dir so its own
# CMakeLists, which would probe OpenSSL/zlib/brotli, is never run) and use httplib.h directly.
include(FetchContent)
FetchContent_Declare(cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.37.2   # commit 4d7c9a788de136071ccf0dd4e96239151e2adadb
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  hcs-no-cmake)
FetchContent_MakeAvailable(cpp_httplib)
set(HCS_HTTPLIB_INCLUDE_DIR "${cpp_httplib_SOURCE_DIR}" CACHE INTERNAL "")
