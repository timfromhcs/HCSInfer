#pragma once
// Read-only random-access byte sources. FileSource uses Win32 overlapped I/O (the handle is
// opened with FILE_FLAG_OVERLAPPED so the streaming pipeline can attach an I/O completion port).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

#include "hcs/result.hpp"

namespace hcs::storage {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual std::uint64_t size() const noexcept = 0;
    // Exact read; a range outside the source or a short read is an error (never a partial success).
    virtual Result<void> read_at(std::uint64_t offset, void* dst, std::size_t n) const = 0;
};

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    std::uint64_t size() const noexcept override { return bytes_.size(); }
    Result<void> read_at(std::uint64_t offset, void* dst, std::size_t n) const override;

private:
    std::span<const std::uint8_t> bytes_;
};

class FileSource final : public ByteSource {
public:
    // Opens read-only (shared read). Never writes, never creates. `sequential_hint` adds
    // FILE_FLAG_SEQUENTIAL_SCAN (an experimental hint; benchmark before enabling by default).
    static Result<std::unique_ptr<FileSource>> open(const std::filesystem::path& path,
                                                    bool sequential_hint = false);
    ~FileSource() override;
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    std::uint64_t size() const noexcept override { return size_; }
    Result<void> read_at(std::uint64_t offset, void* dst, std::size_t n) const override;
    const std::filesystem::path& path() const noexcept { return path_; }
    void* native_handle() const noexcept;  // HANDLE on Windows, nullptr elsewhere
    bool sequential_hint() const noexcept { return sequential_; }

private:
    FileSource() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    bool sequential_ = false;
};

}  // namespace hcs::storage
