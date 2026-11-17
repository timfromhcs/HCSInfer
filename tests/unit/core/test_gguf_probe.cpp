// Adversarial unit tests for the bounded GGUF header probe (AGENTS.md 16.1 C: truncated headers,
// absurd lengths, integer overflow, invalid types). Synthetic files only: no model weights.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hcs/gguf_probe.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using hcs::test::TempDir;
using hcs::test::write_file;

namespace {

// ---- tiny GGUF writer (v2/v3 layout) -------------------------------------------------------
void put_u32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u64(std::string& s, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_str(std::string& s, std::string_view v) {
    put_u64(s, v.size());
    s.append(v);
}

struct Gguf {
    std::string bytes;
    explicit Gguf(std::uint32_t version = 3, std::uint64_t tensors = 0, std::uint64_t kvs = 0) {
        bytes = "GGUF";
        put_u32(bytes, version);
        put_u64(bytes, tensors);
        put_u64(bytes, kvs);
    }
    Gguf& kv_str(std::string_view key, std::string_view value) {
        put_str(bytes, key);
        put_u32(bytes, 8);  // kString
        put_str(bytes, value);
        return *this;
    }
    Gguf& kv_u32(std::string_view key, std::uint32_t value) {
        put_str(bytes, key);
        put_u32(bytes, 4);  // kU32
        put_u32(bytes, value);
        return *this;
    }
    // array of strings
    Gguf& kv_str_array(std::string_view key, const std::vector<std::string>& values) {
        put_str(bytes, key);
        put_u32(bytes, 9);  // kArray
        put_u32(bytes, 8);  // element kString
        put_u64(bytes, values.size());
        for (const auto& v : values) put_str(bytes, v);
        return *this;
    }
    Gguf& kv_raw_array(std::string_view key, std::uint32_t elem_type, std::uint64_t count) {
        put_str(bytes, key);
        put_u32(bytes, 9);
        put_u32(bytes, elem_type);
        put_u64(bytes, count);
        return *this;
    }
    Gguf& tensor(std::string_view name, std::vector<std::uint64_t> dims = {2, 2}, std::uint32_t type = 0) {
        put_str(bytes, name);
        put_u32(bytes, static_cast<std::uint32_t>(dims.size()));
        for (auto d : dims) put_u64(bytes, d);
        put_u32(bytes, type);
        put_u64(bytes, 0);  // offset
        return *this;
    }
};

GgufProbeResult probe(const std::string& s) {
    return probe_gguf(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

}  // namespace

TEST(GgufProbe, TextModelWithArchitecture) {
    Gguf g(3, 1, 2);
    g.kv_str("general.architecture", "llama").kv_u32("llama.block_count", 32).tensor("blk.0.attn_q.weight");
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.complete) << r.reason;
    EXPECT_EQ(r.version, 3u);
    EXPECT_EQ(r.tensor_count, 1u);
    EXPECT_EQ(r.kv_count, 2u);
    EXPECT_EQ(r.architecture, "llama");
    EXPECT_EQ(r.kind, ModelKind::Text);
    EXPECT_EQ(r.tensor_names_read, 1u);
}

TEST(GgufProbe, DiffusionModelIsClassifiedAsImageWithoutMetadata) {
    // stable-diffusion.cpp GGUF v3 files carry no metadata at all: only tensor names identify them.
    Gguf g(3, 3, 0);
    g.tensor("alphas_cumprod", {1000}, 1)
        .tensor("cond_stage_model.transformer.text_model.x", {2, 2})
        .tensor("model.diffusion_model.input_blocks.0.0.weight", {2, 2});
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.complete) << r.reason;
    EXPECT_EQ(r.kind, ModelKind::Image);
    EXPECT_EQ(r.architecture, "stable-diffusion");
}

TEST(GgufProbe, ArchitectureMarksDiffusion) {
    Gguf g(3, 1, 1);
    g.kv_str("general.architecture", "stable-diffusion").tensor("weird.tensor.name");
    const auto r = probe(g.bytes);
    EXPECT_EQ(r.kind, ModelKind::Image);
    EXPECT_EQ(r.architecture, "stable-diffusion");
}

TEST(GgufProbe, UnknownContentStaysUnknown) {
    Gguf g(3, 1, 0);
    g.tensor("something.else");
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.complete) << r.reason;
    EXPECT_EQ(r.kind, ModelKind::Unknown);
    EXPECT_TRUE(r.architecture.empty());
}

TEST(GgufProbe, StringArrayMetadataIsSkipped) {
    Gguf g(3, 1, 1);
    g.kv_str_array("general.tags", {"a", "b", "c"}).tensor("blk.0.weight");
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.complete) << r.reason;
    EXPECT_EQ(r.kind, ModelKind::Text);
}

TEST(GgufProbe, TruncatedTensorTableIsIncompleteNotFatal) {
    Gguf g(3, 5, 0);
    g.tensor("blk.0.weight");
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.valid);
    EXPECT_FALSE(r.complete);
    EXPECT_FALSE(r.reason.empty());
    EXPECT_EQ(r.tensor_names_read, 1u);
}

TEST(GgufProbe, TinyInputsAreRejectedWithoutReading) {
    EXPECT_FALSE(probe("").valid);
    EXPECT_FALSE(probe("GG").valid);
    EXPECT_FALSE(probe("GGUF").valid);
    // magic + version but no counts: the header is not usable yet.
    auto r = probe(std::string("GGUF\x03\0\0\0", 8));
    EXPECT_FALSE(r.valid);
    EXPECT_FALSE(r.complete);
    EXPECT_FALSE(r.reason.empty());
}

TEST(GgufProbe, BadMagicIsRejected) {
    std::string s = "GGUX";
    s.append(64, '\0');
    const auto r = probe(s);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.reason, "invalid GGUF magic");
}

TEST(GgufProbe, UnsupportedVersionIsRejected) {
    Gguf g(99, 0, 0);
    const auto r = probe(g.bytes);
    EXPECT_FALSE(r.valid);
    EXPECT_NE(r.reason.find("unsupported GGUF version"), std::string::npos);
}

TEST(GgufProbe, V1IsValidButNotParsed) {
    Gguf g(1, 0, 0);
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.valid);
    EXPECT_FALSE(r.complete);
    EXPECT_NE(r.reason.find("GGUFv1"), std::string::npos);
}

TEST(GgufProbe, AbsurdStringLengthIsRejected) {
    Gguf g(3, 0, 1);
    put_u64(g.bytes, std::uint64_t{1} << 62);  // key length: far beyond the buffer
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.valid);
    EXPECT_FALSE(r.complete);
    EXPECT_FALSE(r.reason.empty());
}

TEST(GgufProbe, AbsurdArrayCountIsRejectedWithoutAllocating) {
    Gguf g(3, 0, 1);
    g.kv_raw_array("general.tags", 8, std::uint64_t{1} << 40);
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.valid);
    EXPECT_FALSE(r.complete);
    EXPECT_NE(r.reason.find("too many elements"), std::string::npos);
}

TEST(GgufProbe, HugeFixedArrayCountIsRejectedByBufferBounds) {
    Gguf g(3, 0, 1);
    g.kv_raw_array("general.counts", 4, 3);  // declares 3 * 4 bytes, buffer has none
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.valid);
    EXPECT_FALSE(r.complete);
    EXPECT_FALSE(r.reason.empty());
}

TEST(GgufProbe, TooManyTensorDimsIsRejected) {
    Gguf g(3, 1, 0);
    put_str(g.bytes, "x");
    put_u32(g.bytes, 9);  // > 8
    const auto r = probe(g.bytes);
    EXPECT_FALSE(r.valid);
    EXPECT_NE(r.reason.find("too many dimensions"), std::string::npos);
}

TEST(GgufProbe, UnknownMetadataTypeIsRejected) {
    Gguf g(3, 0, 1);
    put_str(g.bytes, "general.weird");
    put_u32(g.bytes, 12345);
    const auto r = probe(g.bytes);
    EXPECT_TRUE(r.valid);
    EXPECT_FALSE(r.complete);
    EXPECT_NE(r.reason.find("unknown metadata value type"), std::string::npos);
}

TEST(GgufProbe, NameCapStopsWalking) {
    Gguf g(3, 3, 0);
    g.tensor("blk.0.weight").tensor("blk.1.weight").tensor("blk.2.weight");
    const auto r = probe_gguf(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(g.bytes.data()),
                                                            g.bytes.size()),
                              2);
    EXPECT_EQ(r.tensor_names_read, 2u);
    EXPECT_FALSE(r.complete);
    EXPECT_EQ(r.kind, ModelKind::Text);
}

TEST(GgufProbe, FileProbeReadsAndReportsMissingFiles) {
    TempDir t;
    Gguf g(3, 1, 1);
    g.kv_str("general.architecture", "qwen2").tensor("blk.0.attn_q.weight");
    write_file(t / "model.gguf", g.bytes);
    const auto ok = probe_gguf_file(t / "model.gguf");
    EXPECT_TRUE(ok.valid);
    EXPECT_TRUE(ok.complete) << ok.reason;
    EXPECT_EQ(ok.architecture, "qwen2");
    EXPECT_EQ(ok.kind, ModelKind::Text);

    const auto missing = probe_gguf_file(t / "nope.gguf");
    EXPECT_FALSE(missing.valid);
    EXPECT_EQ(missing.reason, "cannot open file");
}

TEST(GgufProbe, FileProbeHonoursTheByteLimit) {
    TempDir t;
    Gguf g(3, 1, 0);
    g.tensor("blk.0.weight");
    write_file(t / "model.gguf", g.bytes);
    const auto r = probe_gguf_file(t / "model.gguf", 24);
    EXPECT_FALSE(r.complete);
    EXPECT_EQ(r.tensor_names_read, 0u);}
