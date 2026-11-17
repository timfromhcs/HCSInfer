#pragma once
// Header-only image sniffing for untrusted request images (AGENTS.md 19.3): format + dimensions are read
// from the first bytes WITHOUT decoding any pixel, so size/pixel limits are enforced before a decoder runs.
// Recognises png, jpeg, gif and bmp (the formats the vision adapter accepts).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace hcs::api {

struct ImageProbe {
    std::string format;  // "png" | "jpeg" | "gif" | "bmp"
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t pixels() const noexcept { return static_cast<std::uint64_t>(width) * height; }
};

// nullopt when the bytes are not a recognised image or the header is truncated/inconsistent.
std::optional<ImageProbe> probe_image(const std::uint8_t* data, std::size_t size);

}  // namespace hcs::api
