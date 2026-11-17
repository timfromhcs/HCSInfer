#include "hcs/fs_util.hpp"

#include <atomic>
#include <fstream>
#include <system_error>

namespace hcs {

namespace fs = std::filesystem;

Result<void> atomic_write_file(const fs::path& path, std::string_view content) {
    static std::atomic<std::uint64_t> counter{0};
    std::error_code ec;
    const fs::path parent = path.has_parent_path() ? path.parent_path() : fs::path(".");
    fs::create_directories(parent, ec);
    if (ec)
        return make_error(ErrorCode::IoError,
                          "cannot create directory " + parent.string() + ": " + ec.message());

    fs::path tmp = path;
    tmp += ".tmp." + std::to_string(counter.fetch_add(1));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return make_error(ErrorCode::IoError, "cannot open temp file for " + path.string());
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return make_error(ErrorCode::IoError, "write failed for " + path.string());
        }
    }
    fs::rename(tmp, path, ec);  // replaces the destination (MoveFileEx REPLACE_EXISTING on Windows)
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return make_error(ErrorCode::IoError, "rename failed for " + path.string() + ": " + ec.message());
    }
    return {};
}

Result<std::string> read_file_limited(const fs::path& path, std::uint64_t max_bytes) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec)
        return make_error(ErrorCode::NotFound, "cannot stat " + path.string() + ": " + ec.message());
    if (size > max_bytes) {
        return make_error(ErrorCode::InvalidArgument, "file too large: " + path.string());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return make_error(ErrorCode::IoError, "cannot open " + path.string());
    std::string data(static_cast<std::size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        return make_error(ErrorCode::IoError, "short read on " + path.string());
    }
    return data;
}

}  // namespace hcs
