# Child-process supervision (ADR-011): spawn a helper, capture or stream its output and kill the whole tree.
# Windows implementation (Job Object with kill-on-close, optional process/memory caps); other platforms
# return Unsupported. launcher.cpp is the small IProcessLauncher seam used by the agent session manager.
hcs_add_library(process
  SOURCES
    child_process.cpp launcher.cpp
  PUBLIC_LINK hcs_core)
