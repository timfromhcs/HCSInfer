#pragma once
// HCSMODEL v1: optional chunked, integrity-checked wrapper around a model file (docs/model-vault.md).
// Everything in a container file is untrusted input. All multi-byte fields are little-endian.
//
//   [0, 288)                  header (fixed size, see kHeaderSize)
//   [288, table_offset)       independently decodable chunk payloads
//   [table_offset, +80*count) chunk table
//
// The logical (decoded) stream is the concatenation of all chunks and equals the source file byte
// for byte. The table is placed after the payload so the writer can stream; the header points to it.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "hcs/result.hpp"
#include "hcs/storage/codec.hpp"
#include "hcs/storage/sha256.hpp"

namespace hcs::storage {

inline constexpr char kMagic[8] = {'H', 'C', 'S', 'M', 'O', 'D', 'E', 'L'};
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::uint32_t kHeaderSize = 288;
inline constexpr std::uint32_t kChunkEntrySize = 80;
inline constexpr std::uint32_t kEndianMarker = 0x01020304u;  // stored little-endian: 04 03 02 01
inline constexpr std::uint64_t kMaxChunkCount = 1ull << 20;
inline constexpr std::uint64_t kMaxChunkRawSize = 256ull << 20;  // 256 MiB cap on any declared chunk
inline constexpr std::uint32_t kMinWriterChunkSize = 64u << 10;
inline constexpr std::size_t kIdFieldSize = 64;
inline constexpr std::size_t kTextFieldSize = 32;

struct HcsHeader {
    std::uint32_t format_version = kFormatVersion;
    std::uint32_t nominal_chunk_size = 0;
    std::uint64_t chunk_table_offset = 0;
    std::uint64_t chunk_count = 0;
    std::uint64_t total_raw_size = 0;
    std::string model_id;      // <= 63 bytes
    std::string architecture;  // <= 31 bytes
    std::string quantization;  // <= 31 bytes
    Digest source_manifest_hash{};  // SHA-256 of the complete logical stream
    Digest chunk_table_hash{};      // SHA-256 of the serialized chunk table
};

struct ChunkEntry {
    std::uint64_t region_id = 0;   // opaque (e.g. tensor index); writer sets the chunk index
    std::uint64_t raw_offset = 0;  // position in the logical stream (must be contiguous)
    std::uint64_t raw_size = 0;
    std::uint64_t offset = 0;      // file offset of the stored payload
    std::uint64_t comp_size = 0;   // stored size (== raw_size for Raw)
    Codec codec = Codec::Raw;
    std::uint32_t flags = 0;       // must be 0 in v1
    Digest hash{};                 // SHA-256 of the DECODED chunk
};

struct ParseOptions {
    // Fuzzing may disable the two checksum comparisons to reach deeper structural checks.
    bool verify_checksums = true;
};

// --- Pure parsers (no I/O). Validate everything before any allocation sized by file data. ---
Result<HcsHeader> parse_header(std::span<const std::uint8_t> bytes, std::uint64_t file_size,
                               const ParseOptions& opt = {});
Result<std::vector<ChunkEntry>> parse_chunk_table(std::span<const std::uint8_t> table, const HcsHeader& header,
                                                  std::uint64_t file_size, const ParseOptions& opt = {});

struct ParsedContainer {
    HcsHeader header;
    std::vector<ChunkEntry> chunks;
};
// Parses header + table from a complete in-memory image (file_size = image.size()).
Result<ParsedContainer> parse_container_image(std::span<const std::uint8_t> image, const ParseOptions& opt = {});

// --- Serialization (writer + tests). serialize_header fills the trailing header hash. ---
std::vector<std::uint8_t> serialize_header(const HcsHeader& h);
void serialize_chunk_entry(const ChunkEntry& e, std::uint8_t* out /* kChunkEntrySize bytes */);
std::vector<std::uint8_t> serialize_chunk_table(const std::vector<ChunkEntry>& entries);

// Decodes one stored chunk into `raw_out` (exactly entry.raw_size bytes) and, if `verify`, checks
// the SHA-256. Corrupt data / size mismatch / hash mismatch is a ModelError.
Result<void> decode_chunk(const ChunkEntry& entry, std::span<const std::uint8_t> stored,
                          std::span<std::uint8_t> raw_out, bool verify);

}  // namespace hcs::storage
