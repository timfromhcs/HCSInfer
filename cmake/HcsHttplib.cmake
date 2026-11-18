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

# Local patch (soak defect D-S4, request deadline): httplib::Server::process_and_close_socket is a private
# virtual, so a subclass can override it but never call the base implementation. HttpServer needs exactly that
# to track each connection (cpp-httplib only has per-read timeouts, so a client that drips header bytes could
# hold a worker forever). The ONLY change is one access specifier (private -> protected) for that one member,
# applied to a copy of the header in the build tree; the fetched source stays pristine and every translation
# unit sees the same patched header, so there is no ODR/layout difference.
file(READ "${cpp_httplib_SOURCE_DIR}/httplib.h" _hcs_httplib_text)
set(_hcs_httplib_decl "  virtual bool process_and_close_socket(socket_t sock);
")
string(FIND "${_hcs_httplib_text}" "${_hcs_httplib_decl}" _hcs_httplib_pos)
if(_hcs_httplib_pos EQUAL -1)
  message(FATAL_ERROR "cpp-httplib ${CMAKE_CURRENT_LIST_FILE}: process_and_close_socket declaration not found; "
                      "the HCS access patch must be re-validated for this httplib version")
endif()
string(REPLACE "${_hcs_httplib_decl}"
  "protected:  /* HCS patch: was private */
${_hcs_httplib_decl}private:
"
  _hcs_httplib_text "${_hcs_httplib_text}")
set(_hcs_httplib_dir "${CMAKE_BINARY_DIR}/hcs-httplib-patched")
file(MAKE_DIRECTORY "${_hcs_httplib_dir}")
file(WRITE "${_hcs_httplib_dir}/httplib.h" "${_hcs_httplib_text}")
unset(_hcs_httplib_text)
set(HCS_HTTPLIB_INCLUDE_DIR "${_hcs_httplib_dir}" CACHE INTERNAL "")
