// `HCSInfer cache stats|clear`.
//
// Scope (everything else is state or user data and is never touched):
//   * <state>/cache    - reserved for HCS-owned, regenerable caches (nothing in this build writes there yet)
//   * <state>/scratch  - reserved for HCS-owned scratch files (nothing in this build writes there yet)
//   * orphaned atomic-write leftovers "<name>.tmp.<n>" in <state> and <state>/train (an interrupted
//     atomic_write_file; the only cache-like files HCS really leaves behind today)
// Model files, registry.json, settings.json and train manifests are not in scope. Links/junctions are never
// followed, and every removal is checked with PathGuard against the state directory first.
#include "cache_cmd.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <ostream>
#include <system_error>

#include "hcs/path_guard.hpp"
#include "hcs/result.hpp"

namespace hcs::cli {

namespace fs = std::filesystem;
using nlohmann::json;

const char* cache_help() {
    return "Usage: HCSInfer cache <subcommand>\n"
           "\n"
           "  stats [--json]        Count what HCS itself keeps in its state directory as cache\n"
           "  clear --yes [--json]  Delete exactly that (nothing is deleted without --yes)\n"
           "\n"
           "Scope - only HCS-owned files in the state directory (--state-dir, $HCS_DATA_DIR, $HCS_STATE_DIR "
           "or the "
           "default):\n"
           "  cache/                reserved for HCS-owned caches (empty unless a feature writes there)\n"
           "  scratch/              reserved for HCS-owned scratch files (empty unless a feature writes "
           "there)\n"
           "  <name>.tmp.<n>        orphaned leftovers of an interrupted atomic write, in the state "
           "directory\n"
           "                        and in train/\n"
           "Never touched: model files and model folders, registry.json, settings.json, train manifests and\n"
           "adapters, anything outside the state directory. Links and junctions are skipped, not followed.\n"
           "HCS creates no decompressed model cache or staging file (docs/model-vault.md), so on a normal\n"
           "installation `cache stats` honestly reports 0 entries.\n"
           "Exit codes: 0 ok, 2 invalid argument (e.g. clear without --yes), 8 some entries could not be "
           "removed.\n";
}

namespace {

constexpr std::size_t kMaxDepth = 16;
constexpr std::size_t kMaxVisited = 200000;

bool has_flag(std::vector<std::string>& v, const std::string& flag) {
    auto it = std::find(v.begin(), v.end(), flag);
    if (it == v.end())
        return false;
    v.erase(it);
    return true;
}

bool wants_help(const std::vector<std::string>& v) {
    return std::any_of(v.begin(), v.end(), [](const std::string& a) { return a == "--help" || a == "-h"; });
}

int fail(std::ostream& err, ErrorCode c, const std::string& m) {
    err << "error: " << m << " [" << to_string(c) << "]\n";
    return exit_code(c);
}

bool is_link(const fs::file_status& st) {
#ifdef _WIN32
    return st.type() == fs::file_type::symlink || st.type() == fs::file_type::junction;
#else
    return st.type() == fs::file_type::symlink;  // file_type::junction is an MSVC STL extension
#endif
}

// "<name>.tmp.<digits>" with a non-empty <name>: the exact shape atomic_write_file leaves behind.
bool is_orphaned_temp_name(const std::string& name) {
    const auto pos = name.rfind(".tmp.");
    if (pos == std::string::npos || pos == 0)
        return false;
    const std::string digits = name.substr(pos + 5);
    return !digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos;
}

struct Group {
    Group() = default;
    explicit Group(std::string n) : name(std::move(n)) {}
    std::string name;
    bool exists = false;
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
    std::vector<fs::path> files;
    std::vector<fs::path> dirs;  // visited sub-directories, for empty-directory cleanup
    std::vector<std::string> skipped;
};

struct Walk {
    std::size_t visited = 0;
    bool truncated = false;
};

// Collects regular files below `dir` (never following links). `pred` selects which files count.
template <class Pred>
void collect(const fs::path& dir, const fs::path& base, Group& g, Walk& w, std::size_t depth, bool recurse,
             const Pred& pred) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec)
            return;
        if (++w.visited > kMaxVisited) {
            w.truncated = true;
            return;
        }
        std::error_code sec;
        const auto st = it->symlink_status(sec);
        if (sec)
            continue;
        const fs::path p = it->path();
        const std::string rel = p.lexically_relative(base).generic_string();
        if (is_link(st)) {
            if (pred(p, true))
                g.skipped.push_back(rel + " (link, not followed)");
            continue;
        }
        if (st.type() == fs::file_type::directory) {
            if (recurse && depth < kMaxDepth) {
                g.dirs.push_back(p);
                collect(p, base, g, w, depth + 1, recurse, pred);
            }
            continue;
        }
        if (st.type() != fs::file_type::regular) {
            if (pred(p, true))
                g.skipped.push_back(rel + " (not a regular file)");
            continue;
        }
        if (!pred(p, false))
            continue;
        std::error_code zec;
        const auto size = fs::file_size(p, zec);
        ++g.entries;
        g.bytes += zec ? 0 : static_cast<std::uint64_t>(size);
        g.files.push_back(p);
    }
}

struct Scan {
    Group cache{"cache"};
    Group scratch{"scratch"};
    Group temp{"orphaned_temp_files"};
    bool state_exists = false;
    bool truncated = false;
};

bool is_real_dir(const fs::path& p) {
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    return !ec && st.type() == fs::file_type::directory;
}

Scan scan_state(const fs::path& state) {
    Scan s;
    s.state_exists = is_real_dir(state);
    if (!s.state_exists)
        return s;
    Walk w;
    const auto always = [](const fs::path&, bool) { return true; };
    for (Group* g : {&s.cache, &s.scratch}) {
        const fs::path dir = state / g->name;
        std::error_code ec;
        const auto st = fs::symlink_status(dir, ec);
        if (ec || st.type() == fs::file_type::not_found)
            continue;
        g->exists = true;
        if (!is_real_dir(dir)) {  // a link or a file named like the cache directory: report, never follow
            g->skipped.push_back(g->name + " (link or not a directory, not followed)");
            continue;
        }
        collect(dir, dir, *g, w, 0, true, always);
    }
    s.temp.exists = true;
    const auto temp_only = [](const fs::path& p, bool) {
        return is_orphaned_temp_name(p.filename().string());
    };
    collect(state, state, s.temp, w, 0, false, temp_only);
    if (is_real_dir(state / "train")) {
        Group train;
        collect(state / "train", state, train, w, 0, true, temp_only);
        s.temp.entries += train.entries;
        s.temp.bytes += train.bytes;
        s.temp.files.insert(s.temp.files.end(), train.files.begin(), train.files.end());
        s.temp.skipped.insert(s.temp.skipped.end(), train.skipped.begin(), train.skipped.end());
    }
    s.truncated = w.truncated;
    return s;
}

json group_json(const Group& g) {
    return {{"name", g.name},
            {"kind", g.name == "orphaned_temp_files" ? "files" : "directory"},
            {"exists", g.exists},
            {"entries", g.entries},
            {"bytes", g.bytes}};
}

constexpr const char* kNote =
    "HCS keeps no model or decompression cache. Counted: files in <state>/cache and <state>/scratch and "
    "orphaned '<name>.tmp.<n>' atomic-write leftovers in <state> and <state>/train. Model files, "
    "registry.json, settings.json and train manifests are never counted or removed.";

int cmd_stats(std::vector<std::string> args, const fs::path& state, std::ostream& out, std::ostream& err) {
    const bool as_json = has_flag(args, "--json");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    const Scan s = scan_state(state);
    const std::uint64_t entries = s.cache.entries + s.scratch.entries + s.temp.entries;
    const std::uint64_t bytes = s.cache.bytes + s.scratch.bytes + s.temp.bytes;
    if (as_json) {
        json j;
        j["schema_version"] = 1;
        j["state_dir_exists"] = s.state_exists;
        j["entries"] = entries;
        j["bytes"] = bytes;
        j["locations"] = json::array({group_json(s.cache), group_json(s.scratch), group_json(s.temp)});
        json skipped = json::array();
        for (const Group* g : {&s.cache, &s.scratch, &s.temp})
            for (const auto& x : g->skipped) skipped.push_back(x);
        j["skipped"] = std::move(skipped);
        j["truncated"] = s.truncated;
        j["note"] = kNote;
        out << j.dump(2) << "\n";
        return 0;
    }
    out << "HCSInfer cache (state directory " << (s.state_exists ? "present" : "not created yet") << ")\n";
    for (const Group* g : {&s.cache, &s.scratch, &s.temp})
        out << "  " << g->name << ": " << g->entries << " files, " << g->bytes << " bytes\n";
    out << "  total: " << entries << " files, " << bytes << " bytes\n";
    if (s.truncated)
        out << "  (scan truncated: too many entries)\n";
    out << kNote << "\n";
    return 0;
}

int cmd_clear(std::vector<std::string> args, const fs::path& state, std::ostream& out, std::ostream& err) {
    const bool as_json = has_flag(args, "--json");
    const bool yes = has_flag(args, "--yes");
    if (!args.empty())
        return fail(err, ErrorCode::InvalidArgument, "unexpected argument: " + args.front());
    const Scan s = scan_state(state);
    const std::uint64_t entries = s.cache.entries + s.scratch.entries + s.temp.entries;
    const std::uint64_t bytes = s.cache.bytes + s.scratch.bytes + s.temp.bytes;
    if (!yes)
        return fail(err, ErrorCode::InvalidArgument,
                    "cache clear would delete " + std::to_string(entries) + " files (" +
                        std::to_string(bytes) +
                        " bytes) from the HCS state directory; nothing was removed. "
                        "Run `cache stats` to see them and pass --yes to confirm");

    std::uint64_t removed = 0, removed_bytes = 0;
    json failed = json::array(), skipped = json::array();
    for (const Group* g : {&s.cache, &s.scratch, &s.temp})
        for (const auto& x : g->skipped) skipped.push_back(x);

    if (s.state_exists) {
        PathGuard guard;
        if (auto r = guard.add_root(state); !r)
            return fail(err, r.error().code, r.error().message);
        for (const Group* g : {&s.cache, &s.scratch, &s.temp}) {
            for (const auto& f : g->files) {
                const std::string rel = f.lexically_relative(state).generic_string();
                auto resolved = guard.resolve(f);
                if (!resolved) {
                    failed.push_back({{"path", rel}, {"message", resolved.error().message}});
                    continue;
                }
                std::error_code ec;
                const auto size = fs::file_size(resolved.value(), ec);
                const auto bytes_here = ec ? std::uint64_t{0} : static_cast<std::uint64_t>(size);
                ec.clear();
                if (!fs::remove(resolved.value(), ec) || ec) {
                    failed.push_back({{"path", rel}, {"message", ec ? ec.message() : "could not remove"}});
                    continue;
                }
                ++removed;
                removed_bytes += bytes_here;
            }
            // Remove sub-directories that became empty (deepest first); the cache/scratch roots stay.
            std::vector<fs::path> dirs = g->dirs;
            std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
                return a.native().size() > b.native().size();
            });
            for (const auto& d : dirs) {
                if (g == &s.temp)
                    continue;  // never remove train/ sub-directories
                auto resolved = guard.resolve(d);
                if (!resolved)
                    continue;
                std::error_code ec;
                if (fs::is_empty(resolved.value(), ec) && !ec)
                    fs::remove(resolved.value(), ec);  // fs::remove only removes an empty directory
            }
        }
    }
    if (as_json) {
        json j;
        j["schema_version"] = 1;
        j["removed_entries"] = removed;
        j["removed_bytes"] = removed_bytes;
        j["skipped"] = std::move(skipped);
        j["failed"] = failed;
        out << j.dump(2) << "\n";
    } else {
        out << "removed " << removed << " files (" << removed_bytes << " bytes)\n";
        for (const auto& x : skipped) out << "  skipped: " << x.get<std::string>() << "\n";
        for (const auto& f : failed)
            out << "  failed: " << f["path"].get<std::string>() << ": " << f["message"].get<std::string>()
                << "\n";
    }
    if (!failed.empty()) {
        err << "error: " << failed.size() << " entries could not be removed [io_error]\n";
        return exit_code(ErrorCode::IoError);
    }
    return 0;
}

}  // namespace

int run_cache(std::vector<std::string> args, const fs::path& state_dir, std::ostream& out,
              std::ostream& err) {
    if (args.empty()) {
        err << cache_help();
        return exit_code(ErrorCode::InvalidArgument);
    }
    const std::string sub = args.front();
    args.erase(args.begin());
    if (sub == "--help" || sub == "-h") {
        out << cache_help();
        return 0;
    }
    if (sub != "stats" && sub != "clear")
        return fail(err, ErrorCode::InvalidArgument, "unknown cache subcommand: " + sub);
    if (wants_help(args)) {
        out << cache_help();
        return 0;
    }
    return sub == "stats" ? cmd_stats(std::move(args), state_dir, out, err)
                          : cmd_clear(std::move(args), state_dir, out, err);
}

}  // namespace hcs::cli
