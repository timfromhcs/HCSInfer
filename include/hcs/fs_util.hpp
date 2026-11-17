#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "hcs/result.hpp"

namespace hcs {

// Writes `content` to a temp file in the same directory, then renames over `path`
// (replace-on-rename). Creates parent directories. On failure the temp file is removed
// and the previous file content is left untouched.
Result<void> atomic_write_file(const std::filesystem::path& path, std::string_view content);

// Reads at most `max_bytes`; larger files yield InvalidArgument (bounded allocation).
Result<std::string> read_file_limited(const std::filesystem::path& path, std::uint64_t max_bytes);

}  // namespace hcs
