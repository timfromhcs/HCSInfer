#pragma once
// Test helper: unique temp directory, removed on destruction. Only ever removes the directory it created.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace hcs::test {

class TempDir {
public:
    TempDir() : TempDir(std::filesystem::temp_directory_path()) {}
    explicit TempDir(std::filesystem::path base) {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = base / ("hcs-test-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
        path_ = std::filesystem::canonical(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& rel) const { return path_ / rel; }

private:
    std::filesystem::path path_;
};

inline void write_file(const std::filesystem::path& p, const std::string& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

inline std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Minimal valid-looking GGUF file: magic + version 3 + padding.
inline std::string fake_gguf(std::uint32_t version = 3) {
    std::string s = "GGUF";
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((version >> (8 * i)) & 0xFF));
    s.append(64, '\0');
    return s;
}

// Tries a directory symlink, falls back to an NTFS junction (no admin needed). Returns success.
inline bool make_dir_link(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link, ec);
    if (!ec)
        return true;
#ifdef _WIN32
    const std::string cmd =
        "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>nul";
    if (std::system(cmd.c_str()) == 0)
        return std::filesystem::exists(link, ec);
#endif
    return false;
}

}  // namespace hcs::test
