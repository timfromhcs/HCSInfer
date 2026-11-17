#include "hcs/storage/file_source.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

#include "hcs/checked_math.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hcs::storage {

Result<void> MemorySource::read_at(std::uint64_t offset, void* dst, std::size_t n) const {
    if (!range_within(offset, n, bytes_.size()))
        return Error(ErrorCode::IoError, "read outside of memory source (truncated input)");
    if (n)
        std::memcpy(dst, bytes_.data() + offset, n);
    return {};
}

#ifdef _WIN32

struct FileSource::Impl {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Impl() {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};

namespace {
struct ThreadEvent {
    HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ThreadEvent() {
        if (h)
            CloseHandle(h);
    }
};
HANDLE thread_event() {
    thread_local ThreadEvent ev;
    return ev.h;
}
std::string win_err(const char* what, DWORD code) {
    return std::string(what) + " (Win32 error " + std::to_string(code) + ")";
}
}  // namespace

Result<std::unique_ptr<FileSource>> FileSource::open(const std::filesystem::path& path,
                                                     bool sequential_hint) {
    DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;
    if (sequential_hint)
        flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        const auto code = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ErrorCode::NotFound
                                                                                   : ErrorCode::IoError;
        return Error(code, win_err("cannot open model file", e));
    }
    auto fs = std::unique_ptr<FileSource>(new FileSource());
    fs->impl_ = std::make_unique<Impl>();
    fs->impl_->h = h;
    fs->path_ = path;
    fs->sequential_ = sequential_hint;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || GetFileType(h) != FILE_TYPE_DISK)
        return Error(ErrorCode::IoError, "not a regular disk file");
    fs->size_ = static_cast<std::uint64_t>(sz.QuadPart);
    return fs;
}

FileSource::~FileSource() = default;

void* FileSource::native_handle() const noexcept {
    return impl_ ? impl_->h : nullptr;
}

Result<void> FileSource::read_at(std::uint64_t offset, void* dst, std::size_t n) const {
    if (!range_within(offset, n, size_))
        return Error(ErrorCode::IoError, "read past end of file (truncated)");
    auto* p = static_cast<std::uint8_t*>(dst);
    constexpr std::size_t kMaxPiece = 1u << 30;
    while (n) {
        const DWORD piece = static_cast<DWORD>(std::min<std::size_t>(n, kMaxPiece));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        ov.hEvent = thread_event();
        ResetEvent(ov.hEvent);
        if (!ReadFile(impl_->h, p, piece, nullptr, &ov)) {
            const DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING)
                return Error(ErrorCode::IoError, win_err("read failed", e));
        }
        DWORD got = 0;
        if (!GetOverlappedResult(impl_->h, &ov, &got, TRUE))
            return Error(ErrorCode::IoError, win_err("read failed", GetLastError()));
        if (got != piece)
            return Error(ErrorCode::IoError, "short read (truncated file or device removed)");
        p += piece;
        offset += piece;
        n -= piece;
    }
    return {};
}

#else  // portable fallback (not the target platform; kept compilable)

struct FileSource::Impl {
    mutable std::mutex m;
    mutable std::ifstream in;
};

Result<std::unique_ptr<FileSource>> FileSource::open(const std::filesystem::path& path,
                                                     bool sequential_hint) {
    auto fs = std::unique_ptr<FileSource>(new FileSource());
    fs->impl_ = std::make_unique<Impl>();
    fs->impl_->in.open(path, std::ios::binary);
    if (!fs->impl_->in)
        return Error(ErrorCode::NotFound, "cannot open model file");
    std::error_code ec;
    fs->size_ = std::filesystem::file_size(path, ec);
    if (ec)
        return Error(ErrorCode::IoError, "cannot stat model file");
    fs->path_ = path;
    fs->sequential_ = sequential_hint;
    return fs;
}
FileSource::~FileSource() = default;
void* FileSource::native_handle() const noexcept {
    return nullptr;
}
Result<void> FileSource::read_at(std::uint64_t offset, void* dst, std::size_t n) const {
    if (!range_within(offset, n, size_))
        return Error(ErrorCode::IoError, "read past end of file (truncated)");
    std::lock_guard lk(impl_->m);
    impl_->in.clear();
    impl_->in.seekg(static_cast<std::streamoff>(offset));
    impl_->in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (static_cast<std::size_t>(impl_->in.gcount()) != n)
        return Error(ErrorCode::IoError, "short read");
    return {};
}

#endif

}  // namespace hcs::storage
