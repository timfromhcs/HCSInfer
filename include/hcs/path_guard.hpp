#pragma once
// Approved-root path guard. Every filesystem path that originates from CLI/API/MCP/config
// must pass through resolve() before it is opened.
//
// Rules: absolute-or-relative input is canonicalised (symlinks/junctions resolved for the
// existing prefix); any ".." component, NUL, Windows device/namespace prefix or alternate
// data stream is rejected up front; the canonical result must lie inside an approved root
// (component-wise, case-insensitive on Windows).
#include <filesystem>
#include <vector>

#include "hcs/result.hpp"

namespace hcs {

class PathGuard {
public:
    // Root must exist and be a directory. Stored in canonical form.
    Result<void> add_root(const std::filesystem::path& root);
    // Returns the canonical path (inside a root) or SecurityViolation / InvalidArgument.
    Result<std::filesystem::path> resolve(const std::filesystem::path& candidate) const;

    bool empty() const noexcept { return roots_.empty(); }
    const std::vector<std::filesystem::path>& roots() const noexcept { return roots_; }

private:
    std::vector<std::filesystem::path> roots_;
};

}  // namespace hcs
