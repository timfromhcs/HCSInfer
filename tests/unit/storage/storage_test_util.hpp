#pragma once
// Helpers shared by the storage tests: deterministic data, in-memory HCSMODEL image builder.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "hcs/storage/codec.hpp"
#include "hcs/storage/hcsmodel.hpp"
#include "hcs/storage/sha256.hpp"

namespace hcs::storage::test {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

inline std::vector<std::uint8_t> random_bytes(std::size_t n, std::uint64_t seed) {
    Rng r(seed);
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) b = static_cast<std::uint8_t>(r.next() >> 24);
    return v;
}

// Highly compressible: short repeating pattern with a little noise.
inline std::vector<std::uint8_t> compressible_bytes(std::size_t n, std::uint64_t seed) {
    Rng r(seed);
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>((i % 61) + ((r.next() >> 40) % 97 == 0 ? 1 : 0));
    return v;
}

struct Parts {
    HcsHeader h;
    std::vector<ChunkEntry> e;
    std::vector<std::uint8_t> payload;       // bytes after the header
    std::vector<std::vector<std::uint8_t>> raw;  // original raw chunks
};

// Builds `n` chunks of `chunk` bytes each with the given codec (Raw chunks stay Raw).
inline Parts make_parts(std::size_t n, std::size_t chunk, Codec codec, bool compressible = true, std::uint64_t seed = 1) {
    Parts p;
    p.h.nominal_chunk_size = static_cast<std::uint32_t>(chunk);
    p.h.model_id = "unit-test";
    p.h.architecture = "test";
    p.h.quantization = "none";
    std::uint64_t raw_off = 0;
    Sha256 manifest;
    for (std::size_t i = 0; i < n; ++i) {
        auto raw = compressible ? compressible_bytes(chunk, seed + i) : random_bytes(chunk, seed + i);
        manifest.update(raw);
        ChunkEntry e;
        e.region_id = i;
        e.raw_offset = raw_off;
        e.raw_size = raw.size();
        e.hash = sha256(raw);
        e.codec = codec;
        std::vector<std::uint8_t> stored;
        if (codec == Codec::Raw) {
            stored = raw;
        } else {
            (void)compress(codec, 0, raw, stored);
        }
        e.comp_size = stored.size();
        e.offset = kHeaderSize + p.payload.size();
        p.payload.insert(p.payload.end(), stored.begin(), stored.end());
        raw_off += raw.size();
        p.e.push_back(e);
        p.raw.push_back(std::move(raw));
    }
    p.h.total_raw_size = raw_off;
    p.h.chunk_count = n;
    p.h.source_manifest_hash = manifest.finalize();
    return p;
}

// Serializes parts into a full image. fix_* recompute derived fields so a test can corrupt exactly one thing.
inline std::vector<std::uint8_t> assemble(Parts p, bool fix_table_offset = true, bool fix_table_hash = true) {
    const auto table = serialize_chunk_table(p.e);
    if (fix_table_offset) p.h.chunk_table_offset = kHeaderSize + p.payload.size();
    if (fix_table_hash) p.h.chunk_table_hash = sha256(table);
    auto img = serialize_header(p.h);
    img.insert(img.end(), p.payload.begin(), p.payload.end());
    img.insert(img.end(), table.begin(), table.end());
    return img;
}

inline void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& b) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

inline std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---- GGUF builder ---------------------------------------------------------------------------
struct GgufBuilder {
    std::vector<std::uint8_t> b;
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void str(const std::string& s) { u64(s.size()); b.insert(b.end(), s.begin(), s.end()); }
    void raw(const char* s, std::size_t n) { b.insert(b.end(), s, s + n); }
};

// Valid tiny GGUF: 2 tensors (F32 [8] and Q8_0 [64, 2]) and 3 metadata entries.
inline std::vector<std::uint8_t> make_gguf(std::uint32_t version = 3) {
    GgufBuilder g;
    g.raw("GGUF", 4);
    g.u32(version);
    g.u64(2);  // tensors
    g.u64(3);  // kv
    g.str("general.architecture"); g.u32(8); g.str("testarch");
    g.str("general.name"); g.u32(8); g.str("tiny");
    g.str("tokenizer.tokens"); g.u32(9); g.u32(8); g.u64(3); g.str("a"); g.str("bb"); g.str("ccc");
    // tensor infos
    g.str("t0"); g.u32(1); g.u64(8); g.u32(0); g.u64(0);          // F32 x8  = 32 bytes @0
    g.str("t1"); g.u32(2); g.u64(64); g.u64(2); g.u32(8); g.u64(32);  // Q8_0 [64,2] = 2*2*34 = 136 bytes @32 (aligned to 32)
    while (g.b.size() % 32) g.b.push_back(0);
    const std::size_t data = g.b.size();
    g.b.resize(data + 32 + 136, 0xAB);
    return g.b;
}

}  // namespace hcs::storage::test
