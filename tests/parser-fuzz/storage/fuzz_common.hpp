#pragma once
// Shared fuzz entry points: used by the libFuzzer harness and the deterministic mutation smoke test.
// Contract: never crash, never read out of bounds, and on success all invariants hold.
#include <cstdint>
#include <span>
#include <vector>

#include "hcs/checked_math.hpp"
#include "hcs/storage/file_source.hpp"
#include "hcs/storage/gguf.hpp"
#include "hcs/storage/hcsmodel.hpp"

namespace hcs::storage::fuzz {

// Returns true if the container parsed; aborts (via return false from invariant check) on violation.
inline bool check_container(std::span<const std::uint8_t> data, bool verify_checksums, bool* invariants_ok) {
    *invariants_ok = true;
    auto r = parse_container_image(data, ParseOptions{verify_checksums});
    if (!r.ok())
        return false;
    const auto& p = r.value();
    std::uint64_t running = 0;
    for (const auto& e : p.chunks) {
        if (e.raw_offset != running || e.raw_size == 0 || e.raw_size > kMaxChunkRawSize ||
            !range_within(e.offset, e.comp_size, data.size()) || e.offset < kHeaderSize)
            *invariants_ok = false;
        running += e.raw_size;
        // Decode the chunk (bounded by the validated raw size) - must fail cleanly or succeed, never crash.
        if (e.raw_size <= (4u << 20) && range_within(e.offset, e.comp_size, data.size())) {
            std::vector<std::uint8_t> out(static_cast<std::size_t>(e.raw_size));
            (void)decode_chunk(
                e, data.subspan(static_cast<std::size_t>(e.offset), static_cast<std::size_t>(e.comp_size)),
                out, true);
        }
    }
    if (running != p.header.total_raw_size)
        *invariants_ok = false;
    return true;
}

inline bool check_gguf(std::span<const std::uint8_t> data, bool* invariants_ok) {
    *invariants_ok = true;
    MemorySource src(data);
    auto r = parse_gguf(src);
    if (!r.ok())
        return false;
    const auto& g = r.value();
    if (g.data_offset > data.size() || g.tensors.size() != g.tensor_count)
        *invariants_ok = false;
    for (const auto& t : g.tensors)
        if (t.size_known && !range_within(g.data_offset + t.offset, t.byte_size, data.size()))
            *invariants_ok = false;
    return true;
}

}  // namespace hcs::storage::fuzz
