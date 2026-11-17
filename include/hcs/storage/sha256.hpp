#pragma once
// SHA-256 (FIPS 180-4). Portable implementation plus an x86 SHA-NI path selected at run time.
// Used for per-chunk integrity, the container header/table hashes and the source manifest hash.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace hcs::storage {

using Digest = std::array<std::uint8_t, 32>;

class Sha256 {
public:
    Sha256() noexcept { reset(); }
    void reset() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    void update(std::span<const std::uint8_t> data) noexcept { update(data.data(), data.size()); }
    Digest finalize() noexcept;  // leaves the object reset

private:
    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::uint64_t total_ = 0;
    std::size_t buffered_ = 0;
};

Digest sha256(std::span<const std::uint8_t> data) noexcept;
std::string to_hex(const Digest& d);
bool digest_from_hex(const std::string& hex, Digest& out) noexcept;

// True when the CPU supports the SHA extensions (and they are not disabled below).
bool sha256_hardware_available() noexcept;
// Test/diagnostic switch: force the portable implementation process-wide.
void sha256_force_portable(bool force) noexcept;
// Name of the implementation currently used: "sha-ni" or "portable".
const char* sha256_implementation() noexcept;

}  // namespace hcs::storage
