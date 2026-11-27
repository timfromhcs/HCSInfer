#include "hcs/state_dir.hpp"

#include <cstdlib>

namespace hcs {

namespace fs = std::filesystem;

std::string process_env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string();
}

fs::path resolve_state_dir(const std::string& override_dir, const EnvLookup& env) {
    if (!override_dir.empty())
        return fs::path(override_dir);
    if (auto d = env("HCS_DATA_DIR"); !d.empty())
        return fs::path(d);
    if (auto s = env("HCS_STATE_DIR"); !s.empty())
        return fs::path(s);
    if (auto l = env("LOCALAPPDATA"); !l.empty())
        return fs::path(l) / "HCSInfer";
#if defined(__APPLE__)
    if (auto h = env("HOME"); !h.empty())
        return fs::path(h) / "Library" / "Application Support" / "HCSInfer";
#elif !defined(_WIN32)
    // XDG Base Directory spec: $XDG_STATE_HOME (only an absolute path is valid), else ~/.local/state.
    if (auto x = env("XDG_STATE_HOME"); !x.empty() && fs::path(x).is_absolute())
        return fs::path(x) / "hcsinfer";
    if (auto h = env("HOME"); !h.empty())
        return fs::path(h) / ".local" / "state" / "hcsinfer";
#endif
    if (auto h = env("HOME"); !h.empty())
        return fs::path(h) / ".hcsinfer";
    return fs::path(".hcsinfer");
}

fs::path resolve_state_dir(const std::string& override_dir) {
    return resolve_state_dir(override_dir, process_env);
}

}  // namespace hcs
