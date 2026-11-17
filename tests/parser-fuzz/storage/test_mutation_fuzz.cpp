// Deterministic mutation-fuzz smoke test (runs everywhere, incl. MSVC where libFuzzer is unavailable).
// It is NOT a replacement for coverage-guided fuzzing (see hcsmodel_fuzz.cpp); it proves that a few
// tens of thousands of byte flips / truncations / splices never crash the parsers or violate invariants.
#include <gtest/gtest.h>

#include "fuzz_common.hpp"
#include "storage_test_util.hpp"

using namespace hcs::storage;
namespace tst = hcs::storage::test;

namespace {
std::vector<std::uint8_t> mutate(const std::vector<std::uint8_t>& seed, tst::Rng& rng) {
    auto m = seed;
    const int ops = 1 + static_cast<int>(rng.next() % 4);
    for (int i = 0; i < ops && !m.empty(); ++i) {
        switch (rng.next() % 6) {
            case 0: m[rng.next() % m.size()] ^= static_cast<std::uint8_t>(1u << (rng.next() % 8)); break;
            case 1: m[rng.next() % m.size()] = static_cast<std::uint8_t>(rng.next()); break;
            case 2: m.resize(rng.next() % (m.size() + 1)); break;  // truncate
            case 3: {                                              // overwrite a u64 with an extreme value
                const std::size_t pos = rng.next() % m.size();
                static constexpr std::uint64_t ext[] = {0, 1, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFull, 1ull << 40, 0x7FFFFFFFFFFFFFFFull};
                const std::uint64_t v = ext[rng.next() % 7];
                for (int b = 0; b < 8 && pos + b < m.size(); ++b) m[pos + b] = static_cast<std::uint8_t>(v >> (8 * b));
                break;
            }
            case 4: {  // duplicate a region
                const std::size_t a = rng.next() % m.size(), len = rng.next() % 64;
                std::vector<std::uint8_t> piece(m.begin() + static_cast<std::ptrdiff_t>(a),
                                                m.begin() + static_cast<std::ptrdiff_t>(std::min(m.size(), a + len)));
                m.insert(m.begin() + static_cast<std::ptrdiff_t>(rng.next() % (m.size() + 1)), piece.begin(), piece.end());
                break;
            }
            default: m.insert(m.begin() + static_cast<std::ptrdiff_t>(rng.next() % (m.size() + 1)), static_cast<std::uint8_t>(rng.next())); break;
        }
    }
    return m;
}

// The header/table hashes make most mutations die early; re-seal them so structural checks are reached.
std::vector<std::uint8_t> reseal(std::vector<std::uint8_t> img) {
    if (img.size() < kHeaderSize) return img;
    const auto h = sha256(std::span<const std::uint8_t>(img.data(), 256));
    std::copy(h.begin(), h.end(), img.begin() + 256);
    return img;
}
}  // namespace

TEST(MutationFuzz, HcsModelParserNeverCrashesOrViolatesInvariants) {
    std::vector<std::vector<std::uint8_t>> seeds;
    for (Codec c : {Codec::Raw, Codec::Lz4, Codec::Zstd}) seeds.push_back(tst::assemble(tst::make_parts(5, 4096, c)));
    tst::Rng rng(12345);
    std::size_t parsed = 0, rejected = 0;
    const int iterations = 6000;
    for (int i = 0; i < iterations; ++i) {
        auto m = mutate(seeds[static_cast<std::size_t>(i) % seeds.size()], rng);
        if (i % 2) m = reseal(std::move(m));
        bool inv = true;
        const bool ok = fuzz::check_container(m, /*verify_checksums=*/i % 3 == 0, &inv);
        ASSERT_TRUE(inv) << "invariant violated at iteration " << i;
        (ok ? parsed : rejected)++;
    }
    EXPECT_GT(rejected, 0u);
    RecordProperty("parsed", static_cast<int>(parsed));
    RecordProperty("rejected", static_cast<int>(rejected));
}

TEST(MutationFuzz, GgufParserNeverCrashesOrViolatesInvariants) {
    const auto seed = tst::make_gguf();
    tst::Rng rng(777);
    std::size_t rejected = 0;
    for (int i = 0; i < 8000; ++i) {
        auto m = mutate(seed, rng);
        bool inv = true;
        if (!fuzz::check_gguf(m, &inv)) ++rejected;
        ASSERT_TRUE(inv) << "invariant violated at iteration " << i;
    }
    EXPECT_GT(rejected, 0u);
}

TEST(MutationFuzz, PureRandomGarbageIsRejected) {
    tst::Rng rng(31337);
    for (int i = 0; i < 2000; ++i) {
        auto g = tst::random_bytes(rng.next() % 600, rng.next());
        bool inv = true;
        EXPECT_FALSE(fuzz::check_container(g, true, &inv));
        EXPECT_TRUE(inv);
        EXPECT_FALSE(fuzz::check_gguf(g, &inv));
    }
}
