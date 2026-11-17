# Child-process supervision (ADR-011): spawn a helper, capture its output and kill the whole tree.
# Windows implementation (Job Object with kill-on-close); other platforms return Unsupported.
hcs_add_library(process
  SOURCES
    child_process.cpp
  PUBLIC_LINK hcs_core)
