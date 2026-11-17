#pragma once
// Lossless chunk codecs. Raw = stored; LZ4 and Zstandard are statically linked (see
// THIRD_PARTY_NOTICES.md). All decoders are bounded by the caller-supplied raw size, so a
// malicious chunk can never expand past the declared (and capped) allocation.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "hcs/result.hpp"

namespace hcs::storage {

enum class Codec : std::uint32_t { Raw = 0, Lz4 = 1, Zstd = 2 };

std::string_view codec_name(Codec c) noexcept;
std::optional<Codec> parse_codec(std::string_view name) noexcept;
std::optional<Codec> codec_from_id(std::uint32_t id) noexcept;

// Upper bound of the compressed size of `raw_size` input bytes for `c`.
std::size_t compress_bound(Codec c, std::size_t raw_size) noexcept;

// Compress `src` into `out` (resized to the exact compressed size). `level`: Zstd 1..19
// (default 3); LZ4 <=2 = fast mode, >=3 = LZ4HC at that level (max 12); ignored for Raw.
Result<void> compress(Codec c, int level, std::span<const std::uint8_t> src, std::vector<std::uint8_t>& out);

// Decompress exactly dst.size() bytes. Any size mismatch or corrupt stream is a ModelError.
Result<void> decompress(Codec c, std::span<const std::uint8_t> src, std::span<std::uint8_t> dst);

// Library versions actually linked (for benchmark / evidence metadata).
std::string_view lz4_version() noexcept;
std::string_view zstd_version() noexcept;

}  // namespace hcs::storage
