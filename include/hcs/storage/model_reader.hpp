#pragma once
// Reader abstraction over a model's logical byte stream: a plain GGUF (or any) file or an HCSMODEL
// container. Engines/loaders consume this interface; the Model Vault never writes decoded bytes to disk.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/storage/file_source.hpp"
#include "hcs/storage/gguf.hpp"
#include "hcs/storage/hcsmodel.hpp"
#include "hcs/storage/stream.hpp"

namespace hcs::storage {

class IModelReader {
public:
    virtual ~IModelReader() = default;
    virtual std::string_view kind() const noexcept = 0;  // "gguf" | "raw-file" | "hcsmodel"
    virtual std::uint64_t logical_size() const noexcept = 0;
    // Random access into the logical stream; only the chunks overlapping the range are decoded.
    virtual Result<void> read_range(std::uint64_t offset, std::size_t n, void* dst) const = 0;
    // Sequential, bounded-memory, in-order streaming of the whole logical stream.
    virtual Result<StreamStats> stream(const ChunkSink& sink, const StreamOptions& options) const = 0;
};

class PlainFileReader final : public IModelReader {
public:
    // Opens a file; if it starts with "GGUF" the header is fully validated (failure => error).
    // `chunk_size` is the streaming granularity (the file itself is not modified or copied).
    static Result<std::unique_ptr<PlainFileReader>> open(const std::filesystem::path& path,
                                                         std::uint32_t chunk_size = 16u << 20);
    std::string_view kind() const noexcept override { return gguf_ ? "gguf" : "raw-file"; }
    std::uint64_t logical_size() const noexcept override { return file_->size(); }
    Result<void> read_range(std::uint64_t offset, std::size_t n, void* dst) const override;
    Result<StreamStats> stream(const ChunkSink& sink, const StreamOptions& options) const override;
    const std::optional<GgufInfo>& gguf() const noexcept { return gguf_; }
    const FileSource& file() const noexcept { return *file_; }

private:
    PlainFileReader() = default;
    std::unique_ptr<FileSource> file_;
    std::optional<GgufInfo> gguf_;
    std::uint32_t chunk_size_ = 0;
};

class HcsModelReader final : public IModelReader {
public:
    static Result<std::unique_ptr<HcsModelReader>> open(const std::filesystem::path& path,
                                                        const ParseOptions& opt = {});
    std::string_view kind() const noexcept override { return "hcsmodel"; }
    std::uint64_t logical_size() const noexcept override { return header_.total_raw_size; }
    Result<void> read_range(std::uint64_t offset, std::size_t n, void* dst) const override;
    Result<StreamStats> stream(const ChunkSink& sink, const StreamOptions& options) const override;

    const HcsHeader& header() const noexcept { return header_; }
    const std::vector<ChunkEntry>& chunks() const noexcept { return chunks_; }
    const FileSource& file() const noexcept { return *file_; }
    // Reads, decodes and verifies exactly one chunk (random access; other chunks are not touched).
    Result<void> read_chunk(std::size_t index, std::vector<std::uint8_t>& out) const;

private:
    HcsModelReader() = default;
    std::unique_ptr<FileSource> file_;
    HcsHeader header_;
    std::vector<ChunkEntry> chunks_;
};

// Sniffs the first bytes: "HCSMODEL" -> HcsModelReader, anything else -> PlainFileReader.
Result<std::unique_ptr<IModelReader>> open_model_reader(const std::filesystem::path& path);

}  // namespace hcs::storage
