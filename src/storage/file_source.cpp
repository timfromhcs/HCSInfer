#include "hcs/storage/file_source.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "hcs/checked_math.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
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

#else  // POSIX: open + pread (offset-addressed, thread-safe without a lock)

struct FileSource::Impl {
    int fd = -1;
    ~Impl() {
        if (fd >= 0)
            ::close(fd);
    }
};

namespace {
std::string errno_text(const char* what, int e) {
    return std::string(what) + " (errno " + std::to_string(e) + ": " + std::strerror(e) + ")";
}
}  // namespace

Result<std::unique_ptr<FileSource>> FileSource::open(const std::filesystem::path& path,
                                                     bool sequential_hint) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = -1;
    do {
        fd = ::open(path.c_str(), flags);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        const int e = errno;
        const auto code = (e == ENOENT || e == ENOTDIR) ? ErrorCode::NotFound : ErrorCode::IoError;
        return Error(code, errno_text("cannot open model file", e));
    }
    auto fs = std::unique_ptr<FileSource>(new FileSource());
    fs->impl_ = std::make_unique<Impl>();
    fs->impl_->fd = fd;
    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return Error(ErrorCode::IoError, errno_text("cannot stat model file", errno));
    if (!S_ISREG(st.st_mode))
        return Error(ErrorCode::IoError, "not a regular disk file");
    fs->size_ = static_cast<std::uint64_t>(st.st_size);
    fs->path_ = path;
    fs->sequential_ = sequential_hint;
    if (sequential_hint) {
        // Advisory only: a failure of the hint never fails the open.
#if defined(__linux__)
        (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#elif defined(__APPLE__)
        (void)::fcntl(fd, F_RDAHEAD, 1);
#endif
    }
    return fs;
}

FileSource::~FileSource() = default;

void* FileSource::native_handle() const noexcept {
    return nullptr;  // POSIX has no overlapped-I/O handle; the streaming pipeline uses read_at().
}

Result<void> FileSource::read_at(std::uint64_t offset, void* dst, std::size_t n) const {
    if (!range_within(offset, n, size_))
        return Error(ErrorCode::IoError, "read past end of file (truncated)");
    auto* p = static_cast<std::uint8_t*>(dst);
    constexpr std::size_t kMaxPiece = 1u << 30;  // below the 2 GiB-ish per-call cap of some kernels
    while (n) {
        const std::size_t piece = std::min<std::size_t>(n, kMaxPiece);
        const ssize_t got = ::pread(impl_->fd, p, piece, static_cast<off_t>(offset));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return Error(ErrorCode::IoError, errno_text("read failed", errno));
        }
        if (got == 0)
            return Error(ErrorCode::IoError, "short read (truncated file or device removed)");
        p += got;
        offset += static_cast<std::uint64_t>(got);
        n -= static_cast<std::size_t>(got);
    }
    return {};
}

#endif

}  // namespace hcs::storage
