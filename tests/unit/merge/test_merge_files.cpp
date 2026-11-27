// File-level merge: streaming engine on synthetic safetensors / GGUF files, golden agreement, chunk
// invariance, fail-closed validation, output rules. Synthetic data only (the real-file run is recorded under
// docs/test-evidence).
#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "gguf_builder.hpp"
#include "hcs/merge.hpp"
#include "hcs/safetensors.hpp"
#include "hcs/storage/gguf.hpp"
#include "hcs/storage/sha256.hpp"
#include "safetensors_builder.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using namespace hcs::merge;
using hcs::test::build_gguf;
using hcs::test::build_safetensors;
using hcs::test::f32_bytes;
using hcs::test::f32_tensor;

#include "merge_golden.inc"

namespace {

// The merge engine deliberately refuses outputs on drive C:; use the shared fixture helper so the tests work
// on hosted CI where the system temp is on C:. This changes only the fixture location, never an assertion.
struct Env {
    test::TempDir td{test::safe_temp_base()};
    PathGuard guard;
    Env() { EXPECT_TRUE(guard.add_root(td.path()).ok()); }
    std::filesystem::path p(const std::string& n) const { return td.path() / n; }
};

std::vector<std::uint8_t> f16_bytes(const std::vector<float>& v) {
    std::vector<std::uint8_t> b(v.size() * 2);
    for (std::size_t i = 0; i < v.size(); ++i) {
        const std::uint16_t h = float_to_half(v[i]);
        std::memcpy(&b[i * 2], &h, 2);
    }
    return b;
}

test::StBuildTensor f16_tensor(const std::string& name, std::vector<std::uint64_t> shape,
                               const std::vector<float>& v) {
    test::StBuildTensor t;
    t.name = name;
    t.dtype = "F16";
    t.shape = std::move(shape);
    t.data = f16_bytes(v);
    return t;
}

std::vector<float> read_f32_tensor(const std::filesystem::path& file, const std::string& name) {
    auto h = read_safetensors_header(file);
    EXPECT_TRUE(h.ok());
    const auto* t = h.value().find(name);
    EXPECT_NE(t, nullptr);
    const std::string bytes = test::read_file(file);
    std::vector<float> v(t->element_count());
    std::memcpy(v.data(), bytes.data() + h.value().data_offset + t->begin, v.size() * 4);
    return v;
}

void expect_close(const std::vector<float>& got, const std::vector<float>& want, const char* what) {
    ASSERT_EQ(got.size(), want.size()) << what;
    for (std::size_t i = 0; i < got.size(); ++i)
        EXPECT_NEAR(got[i], want[i], 2e-6 + 1e-5 * std::fabs(want[i])) << what << " [" << i << "]";
}

std::vector<float> random_vec(std::size_t n, unsigned seed, float scale = 1.0f) {
    std::mt19937 g(seed);
    std::normal_distribution<float> d(0.0f, scale);
    std::vector<float> v(n);
    for (auto& x : v) x = d(g);
    return v;
}

Request req(const Env& e, Method m, const std::vector<std::string>& models, const std::string& out) {
    Request r;
    r.base = e.p("base.safetensors");
    for (const auto& n : models) r.models.push_back(e.p(n));
    r.out = e.p(out);
    r.params.method = m;
    return r;
}

void write_three(const Env& e) {
    test::write_file(e.p("base.safetensors"), build_safetensors({f32_tensor("w.test", {4, 8}, kBase)}));
    test::write_file(e.p("m1.safetensors"), build_safetensors({f32_tensor("w.test", {4, 8}, kM1)}));
    test::write_file(e.p("m2.safetensors"), build_safetensors({f32_tensor("w.test", {4, 8}, kM2)}));
}

}  // namespace

TEST(MergeFiles, LinearSlerpTiesDareMatchNumpyGoldens) {
    Env e;
    write_three(e);
    {
        auto r = req(e, Method::Linear, {"m1.safetensors", "m2.safetensors"}, "lin.safetensors");
        r.params.weights = {0.5, 0.3, 0.2};
        auto rep = run_merge(r, e.guard);
        ASSERT_TRUE(rep.ok()) << rep.error().message;
        EXPECT_EQ(rep.value().tensors_merged, 1u);
        expect_close(read_f32_tensor(e.p("lin.safetensors"), "w.test"), kLinear, "linear");
    }
    {
        auto r = req(e, Method::Slerp, {"m1.safetensors"}, "sl.safetensors");
        r.params.t = 0.3;
        ASSERT_TRUE(run_merge(r, e.guard).ok());
        expect_close(read_f32_tensor(e.p("sl.safetensors"), "w.test"), kSlerp, "slerp");
    }
    {
        auto r = req(e, Method::Ties, {"m1.safetensors", "m2.safetensors"}, "ties.safetensors");
        r.params.weights = {1.0, 0.5};
        r.params.density = 0.25;
        r.params.lambda = 0.8;
        ASSERT_TRUE(run_merge(r, e.guard).ok());
        expect_close(read_f32_tensor(e.p("ties.safetensors"), "w.test"), kTies, "ties");
    }
    {
        auto r = req(e, Method::Dare, {"m1.safetensors", "m2.safetensors"}, "dare.safetensors");
        r.params.weights = {1.0, 0.5};
        r.params.drop_rate = 0.25;
        r.params.seed = 99;
        ASSERT_TRUE(run_merge(r, e.guard).ok());
        expect_close(read_f32_tensor(e.p("dare.safetensors"), "w.test"), kDare, "dare");
    }
}

TEST(MergeFiles, LoraIntoBaseMatchesNumpyAndCopiesUntouchedTensors) {
    Env e;
    test::StBuildTensor ids;
    ids.name = "position_ids";
    ids.dtype = "I64";
    ids.shape = {3};
    ids.data.assign(24, 7);
    test::write_file(e.p("base.safetensors"),
                     build_safetensors({f32_tensor("layer.weight", {4, 6}, kLoraBase),
                                        f32_tensor("other.weight", {2}, {1, 2}), ids}));
    test::write_file(e.p("lora.safetensors"),
                     build_safetensors({f32_tensor("base_model.model.layer.lora_A.weight", {2, 6}, kLoraA),
                                        f32_tensor("base_model.model.layer.lora_B.weight", {4, 2}, kLoraB)}));
    auto r = req(e, Method::Lora, {"lora.safetensors"}, "merged.safetensors");
    r.params.weights = {0.75};
    r.params.lora_alpha = 4.0;
    auto rep = run_merge(r, e.guard);
    ASSERT_TRUE(rep.ok()) << rep.error().message;
    EXPECT_EQ(rep.value().tensors_merged, 1u);
    EXPECT_EQ(rep.value().tensors_copied, 2u);
    expect_close(read_f32_tensor(e.p("merged.safetensors"), "layer.weight"), kLoraOut, "lora");
    expect_close(read_f32_tensor(e.p("merged.safetensors"), "other.weight"), {1, 2}, "untouched");
    // The int64 tensor is byte-identical.
    auto h = read_safetensors_header(e.p("merged.safetensors")).value();
    const std::string bytes = test::read_file(e.p("merged.safetensors"));
    EXPECT_EQ(bytes.substr(h.data_offset + h.find("position_ids")->begin, 24), std::string(24, '\7'));
}

TEST(MergeFiles, LoraAlphaTensorAndKohyaAndLlamaNaming) {
    Env e;
    test::write_file(e.p("base.safetensors"),
                     build_safetensors({f32_tensor("layer.weight", {4, 6}, kLoraBase)}));
    // kohya lora_down/lora_up with an alpha tensor of 4 and rank 2 => scale 2 * strength.
    test::write_file(e.p("kohya.safetensors"),
                     build_safetensors({f32_tensor("layer.lora_down.weight", {2, 6}, kLoraA),
                                        f32_tensor("layer.lora_up.weight", {4, 2}, kLoraB),
                                        f32_tensor("layer.alpha", {}, {4.0f})}));
    auto r = req(e, Method::Lora, {"kohya.safetensors"}, "k.safetensors");
    r.params.weights = {0.75};
    ASSERT_TRUE(run_merge(r, e.guard).ok());
    expect_close(read_f32_tensor(e.p("k.safetensors"), "layer.weight"), kLoraOut, "kohya + alpha tensor");
    // llama.cpp style names target the full tensor name; alpha defaults to the rank (scale = strength).
    test::write_file(e.p("llama.safetensors"),
                     build_safetensors({f32_tensor("layer.weight.lora_a", {2, 6}, kLoraA),
                                        f32_tensor("layer.weight.lora_b", {4, 2}, kLoraB)}));
    auto r2 = req(e, Method::Lora, {"llama.safetensors"}, "l.safetensors");
    r2.params.weights = {1.5};  // 1.5 * (rank/rank) == 0.75 * 4/2
    ASSERT_TRUE(run_merge(r2, e.guard).ok());
    expect_close(read_f32_tensor(e.p("l.safetensors"), "layer.weight"), kLoraOut, "llama naming");
}

TEST(MergeFiles, LoraFailsClosed) {
    Env e;
    test::write_file(e.p("base.safetensors"),
                     build_safetensors({f32_tensor("layer.weight", {4, 6}, kLoraBase)}));
    auto run = [&](std::vector<test::StBuildTensor> t) {
        test::write_file(e.p("l.safetensors"), build_safetensors(t));
        auto r = req(e, Method::Lora, {"l.safetensors"}, "o.safetensors");
        r.overwrite = true;
        return run_merge(r, e.guard);
    };
    // unknown target
    EXPECT_EQ(run({f32_tensor("nope.lora_A.weight", {2, 6}, kLoraA),
                   f32_tensor("nope.lora_B.weight", {4, 2}, kLoraB)})
                  .error()
                  .code,
              ErrorCode::ModelError);
    // half a pair
    EXPECT_EQ(run({f32_tensor("layer.lora_A.weight", {2, 6}, kLoraA)}).error().code, ErrorCode::ModelError);
    // rank mismatch
    EXPECT_EQ(run({f32_tensor("layer.lora_A.weight", {2, 6}, kLoraA),
                   f32_tensor("layer.lora_B.weight", {4, 3}, std::vector<float>(12, 1))})
                  .error()
                  .code,
              ErrorCode::ModelError);
    // column mismatch
    EXPECT_EQ(run({f32_tensor("layer.lora_A.weight", {2, 5}, std::vector<float>(10, 1)),
                   f32_tensor("layer.lora_B.weight", {4, 2}, kLoraB)})
                  .error()
                  .code,
              ErrorCode::ModelError);
    // kohya underscore naming cannot be mapped
    EXPECT_EQ(run({f32_tensor("lora_unet_down_blocks_0.lora_down.weight", {2, 6}, kLoraA)}).error().code,
              ErrorCode::Unsupported);
    // no failure left an output or a partial file behind
    EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors")));
    EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors.partial")));
}

TEST(MergeFiles, ChunkSizeNeverChangesTheResult) {
    Env e;
    const std::size_t n = 5000;
    const auto b = random_vec(n, 1), a = random_vec(n, 2), c = random_vec(n, 3);
    test::write_file(e.p("base.safetensors"), build_safetensors({f32_tensor("w", {50, 100}, b),
                                                                 f32_tensor("v", {7}, random_vec(7, 9))}));
    test::write_file(e.p("m1.safetensors"), build_safetensors({f32_tensor("w", {50, 100}, a),
                                                               f32_tensor("v", {7}, random_vec(7, 10))}));
    test::write_file(e.p("m2.safetensors"), build_safetensors({f32_tensor("w", {50, 100}, c),
                                                               f32_tensor("v", {7}, random_vec(7, 11))}));
    for (Method m : {Method::Linear, Method::Slerp, Method::Ties, Method::Dare}) {
        const std::vector<std::string> models =
            m == Method::Slerp ? std::vector<std::string>{"m1.safetensors"}
                               : std::vector<std::string>{"m1.safetensors", "m2.safetensors"};
        std::string first;
        for (std::uint64_t chunk : {1024ull, 3000ull, 1ull << 20}) {
            auto r = req(e, m, models, "o.safetensors");
            r.overwrite = true;
            r.chunk_elements = chunk;
            r.params.density = 0.3;
            r.params.drop_rate = 0.4;
            r.params.seed = 5;
            auto rep = run_merge(r, e.guard);
            ASSERT_TRUE(rep.ok()) << to_string(m) << ": " << rep.error().message;
            const std::string bytes = test::read_file(e.p("o.safetensors"));
            if (first.empty())
                first = bytes;
            else
                EXPECT_EQ(bytes, first) << to_string(m) << " chunk " << chunk;
        }
    }
}

TEST(MergeFiles, MixedDtypesAndBf16RoundToTheBaseDtype) {
    Env e;
    const std::vector<float> a = {1.0f, 2.0f, 3.0f, 4.0f}, b = {3.0f, 2.0f, 1.0f, 0.0f};
    test::write_file(e.p("base.safetensors"), build_safetensors({f16_tensor("w", {4}, a)}));
    test::StBuildTensor bf;
    bf.name = "w";
    bf.dtype = "BF16";
    bf.shape = {4};
    for (float v : b) {
        const std::uint16_t x = float_to_bf16(v);
        bf.data.push_back(static_cast<std::uint8_t>(x & 0xff));
        bf.data.push_back(static_cast<std::uint8_t>(x >> 8));
    }
    test::write_file(e.p("m1.safetensors"), build_safetensors({bf}));
    auto r = req(e, Method::Linear, {"m1.safetensors"}, "o.safetensors");
    ASSERT_TRUE(run_merge(r, e.guard).ok());
    auto h = read_safetensors_header(e.p("o.safetensors")).value();
    EXPECT_EQ(h.find("w")->dtype, "F16");  // base dtype wins
    const std::string bytes = test::read_file(e.p("o.safetensors"));
    for (int i = 0; i < 4; ++i) {
        std::uint16_t x;
        std::memcpy(&x, bytes.data() + h.data_offset + i * 2, 2);
        EXPECT_FLOAT_EQ(half_to_float(x), (a[i] + b[i]) / 2.0f);
    }
}

TEST(MergeFiles, GgufOutputKeepsHeaderAndPaddingAndMergesFloatData) {
    Env e;
    const auto a1 = random_vec(40, 1), a2 = random_vec(5, 2), b1 = random_vec(40, 3), b2 = random_vec(5, 4);
    test::GgufBuildTensor t1{"blk.0.w", 0, {8, 5}, f32_bytes(a1)}, t2{"norm", 1, {5}, f16_bytes(a2)};
    test::GgufBuildTensor u1{"blk.0.w", 0, {8, 5}, f32_bytes(b1)}, u2{"norm", 1, {5}, f16_bytes(b2)};
    test::write_file(e.p("base.gguf"), build_gguf({t1, t2}));
    test::write_file(e.p("m1.gguf"), build_gguf({u1, u2}));
    Request r;
    r.base = e.p("base.gguf");
    r.models = {e.p("m1.gguf")};
    r.out = e.p("out.gguf");
    r.params.method = Method::Linear;
    auto rep = run_merge(r, e.guard);
    ASSERT_TRUE(rep.ok()) << rep.error().message;
    EXPECT_EQ(rep.value().format, "gguf");
    const std::string base_bytes = test::read_file(e.p("base.gguf")), out = test::read_file(e.p("out.gguf"));
    ASSERT_EQ(out.size(), base_bytes.size());
    auto src = storage::FileSource::open(e.p("out.gguf"));
    ASSERT_TRUE(src.ok());
    auto info = storage::parse_gguf(*src.value());
    ASSERT_TRUE(info.ok()) << info.error().message;
    EXPECT_EQ(out.substr(0, info.value().data_offset),
              base_bytes.substr(0, info.value().data_offset));  // header verbatim
    std::vector<float> w(40);
    std::memcpy(w.data(), out.data() + info.value().data_offset + info.value().tensors[0].offset, 160);
    std::vector<float> want(40);
    for (int i = 0; i < 40; ++i) want[i] = (a1[i] + b1[i]) / 2.0f;
    expect_close(w, want, "gguf f32");
    std::uint16_t h0;
    std::memcpy(&h0, out.data() + info.value().data_offset + info.value().tensors[1].offset, 2);
    EXPECT_NEAR(half_to_float(h0),
                (half_to_float(float_to_half(a2[0])) + half_to_float(float_to_half(b2[0]))) / 2.0f, 1e-3);
}

TEST(MergeFiles, QuantizedGgufIsRejectedNotSilentlyCopied) {
    Env e;
    // Q8_0: 34 bytes per 32 elements.
    test::GgufBuildTensor q{"blk.0.w", 8, {32}, std::vector<std::uint8_t>(34, 1)};
    test::write_file(e.p("base.gguf"), build_gguf({q}));
    test::write_file(e.p("m1.gguf"), build_gguf({q}));
    Request r;
    r.base = e.p("base.gguf");
    r.models = {e.p("m1.gguf")};
    r.out = e.p("out.gguf");
    auto rep = run_merge(r, e.guard);
    ASSERT_FALSE(rep.ok());
    EXPECT_EQ(rep.error().code, ErrorCode::Unsupported);
    EXPECT_NE(rep.error().message.find("quantized"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(e.p("out.gguf")));
}

TEST(MergeFiles, LoraIntoGgufBaseKeepsQuantizedUntouchedTensors) {
    Env e;
    test::GgufBuildTensor q{"blk.0.q", 8, {32}, std::vector<std::uint8_t>(34, 3)};
    test::GgufBuildTensor w{"blk.0.w", 0, {6, 4}, f32_bytes(kLoraBase)};
    test::write_file(e.p("base.gguf"), build_gguf({q, w}));
    test::write_file(e.p("lora.safetensors"),
                     build_safetensors({f32_tensor("blk.0.w.lora_a", {2, 6}, kLoraA),
                                        f32_tensor("blk.0.w.lora_b", {4, 2}, kLoraB)}));
    Request r;
    r.base = e.p("base.gguf");
    r.models = {e.p("lora.safetensors")};
    r.out = e.p("out.gguf");
    r.params.method = Method::Lora;
    r.params.weights = {1.5};
    auto rep = run_merge(r, e.guard);
    // Mixed containers are refused: a GGUF base needs a GGUF LoRA (same rule as every other method).
    ASSERT_FALSE(rep.ok());
    EXPECT_EQ(rep.error().code, ErrorCode::InvalidArgument);
    // With a GGUF LoRA the quantized tensor is copied byte for byte and the float one is merged.
    test::GgufBuildTensor la{"blk.0.w.lora_a", 0, {6, 2}, f32_bytes(kLoraA)},
        lb{"blk.0.w.lora_b", 0, {2, 4}, f32_bytes(kLoraB)};
    test::write_file(e.p("lora.gguf"), build_gguf({la, lb}));
    r.models = {e.p("lora.gguf")};
    rep = run_merge(r, e.guard);
    ASSERT_TRUE(rep.ok()) << rep.error().message;
    EXPECT_EQ(rep.value().tensors_merged, 1u);
    EXPECT_EQ(rep.value().tensors_copied, 1u);
    const std::string base_bytes = test::read_file(e.p("base.gguf")), out = test::read_file(e.p("out.gguf"));
    auto src = storage::FileSource::open(e.p("out.gguf"));
    auto info = storage::parse_gguf(*src.value()).value();
    EXPECT_EQ(out.substr(info.data_offset + info.tensors[0].offset, 34),
              base_bytes.substr(info.data_offset + info.tensors[0].offset, 34));
    std::vector<float> got(24);
    std::memcpy(got.data(), out.data() + info.data_offset + info.tensors[1].offset, 96);
    expect_close(got, kLoraOut, "gguf lora");
}

TEST(MergeFiles, StructuralMismatchesFailBeforeAnyOutput) {
    Env e;
    test::write_file(e.p("base.safetensors"),
                     build_safetensors({f32_tensor("w", {4}, {1, 2, 3, 4}), f32_tensor("v", {2}, {1, 2})}));
    auto check = [&](const std::string& other_bytes, ErrorCode code) {
        test::write_file(e.p("m1.safetensors"), other_bytes);
        auto r = req(e, Method::Linear, {"m1.safetensors"}, "o.safetensors");
        auto rep = run_merge(r, e.guard);
        ASSERT_FALSE(rep.ok());
        EXPECT_EQ(rep.error().code, code) << rep.error().message;
        EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors")));
        EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors.partial")));
    };
    check(build_safetensors({f32_tensor("w", {4}, {1, 2, 3, 4})}),
          ErrorCode::InvalidArgument);  // tensor count
    check(build_safetensors({f32_tensor("w", {4}, {1, 2, 3, 4}), f32_tensor("x", {2}, {1, 2})}),
          ErrorCode::InvalidArgument);  // name
    check(build_safetensors({f32_tensor("w", {2, 2}, {1, 2, 3, 4}), f32_tensor("v", {2}, {1, 2})}),
          ErrorCode::InvalidArgument);  // shape
    check("this is not a model", ErrorCode::ModelError);
    check(build_safetensors({f32_tensor("w", {4}, {1, 2, 3, 4})}).substr(0, 40),
          ErrorCode::ModelError);  // truncated
    // GGUF next to safetensors
    check(build_gguf({test::GgufBuildTensor{"w", 0, {4}, f32_bytes({1, 2, 3, 4})}}),
          ErrorCode::InvalidArgument);
}

TEST(MergeFiles, ParameterValidation) {
    Env e;
    write_three(e);
    auto bad = [&](Method m, std::vector<std::string> models, auto tweak) {
        auto r = req(e, m, models, "o.safetensors");
        tweak(r.params);
        auto rep = run_merge(r, e.guard);
        EXPECT_FALSE(rep.ok());
        if (!rep.ok())
            EXPECT_EQ(rep.error().code, ErrorCode::InvalidArgument) << rep.error().message;
        EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors")));
    };
    const auto none = [](Params&) {};
    bad(Method::Linear, {}, none);
    bad(Method::Slerp, {"m1.safetensors", "m2.safetensors"}, none);
    bad(Method::Linear, {"m1.safetensors"}, [](Params& p) { p.weights = {1.0}; });
    bad(Method::Linear, {"m1.safetensors"}, [](Params& p) { p.weights = {1.0, -1.0}; });
    bad(Method::Linear, {"m1.safetensors"}, [](Params& p) { p.weights = {1.0, std::nan("")}; });
    bad(Method::Slerp, {"m1.safetensors"}, [](Params& p) { p.t = 1.5; });
    bad(Method::Ties, {"m1.safetensors"}, [](Params& p) { p.density = 0.0; });
    bad(Method::Ties, {"m1.safetensors"}, [](Params& p) { p.density = 1.5; });
    bad(Method::Dare, {"m1.safetensors"}, [](Params& p) { p.drop_rate = 1.0; });
    bad(Method::Ties, {"m1.safetensors"}, [](Params& p) { p.lambda = 0.0; });
}

TEST(MergeFiles, OutputRules) {
    Env e;
    write_three(e);
    // An existing output is protected unless overwrite is set.
    auto r = req(e, Method::Linear, {"m1.safetensors"}, "o.safetensors");
    ASSERT_TRUE(run_merge(r, e.guard).ok());
    EXPECT_EQ(run_merge(r, e.guard).error().code, ErrorCode::AlreadyExists);
    r.overwrite = true;
    EXPECT_TRUE(run_merge(r, e.guard).ok());
    // The output may not be an input.
    auto self = req(e, Method::Linear, {"m1.safetensors"}, "base.safetensors");
    self.overwrite = true;
    EXPECT_EQ(run_merge(self, e.guard).error().code, ErrorCode::InvalidArgument);
    // Outside every approved root: refused (input and output).
    test::TempDir other;
    auto out_side = req(e, Method::Linear, {"m1.safetensors"}, "x.safetensors");
    out_side.out = other / "x.safetensors";
    EXPECT_EQ(run_merge(out_side, e.guard).error().code, ErrorCode::SecurityViolation);
    auto in_side = req(e, Method::Linear, {"m1.safetensors"}, "x.safetensors");
    test::write_file(other / "m.safetensors", test::read_file(e.p("m1.safetensors")));
    in_side.models = {other / "m.safetensors"};
    EXPECT_EQ(run_merge(in_side, e.guard).error().code, ErrorCode::SecurityViolation);
    // Path traversal in the output.
    auto trav = req(e, Method::Linear, {"m1.safetensors"}, "x.safetensors");
    trav.out = e.td.path() / ".." / "x.safetensors";
    EXPECT_FALSE(run_merge(trav, e.guard).ok());
    EXPECT_FALSE(std::filesystem::exists(e.td.path().parent_path() / "x.safetensors"));
}

#ifdef _WIN32
TEST(MergeFiles, DriveCIsNeverAnOutputEvenInsideAnApprovedRoot) {
    PathGuard g;
    ASSERT_TRUE(g.add_root("C:\\Windows").ok());  // exists on every Windows machine; nothing is written below
    auto r = validate_merge_output("C:\\Windows\\hcs-merge-test.safetensors", g);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation);
    EXPECT_NE(r.error().message.find("C:"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists("C:\\Windows\\hcs-merge-test.safetensors"));
}
#endif

TEST(MergeFiles, MemoryLimitFailsClosedAndCancellationLeavesNothing) {
    Env e;
    const auto v = random_vec(100000, 1);
    test::write_file(e.p("base.safetensors"), build_safetensors({f32_tensor("w", {100000}, v)}));
    test::write_file(e.p("m1.safetensors"),
                     build_safetensors({f32_tensor("w", {100000}, random_vec(100000, 2))}));
    auto r = req(e, Method::Ties, {"m1.safetensors"}, "o.safetensors");
    r.max_ram_bytes = 256u << 10;  // below chunk buffers + the 400 KB selection buffer
    r.chunk_elements = 1024;
    EXPECT_EQ(run_merge(r, e.guard).error().code, ErrorCode::OutOfMemory);
    EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors.partial")));

    CancellationSource cs;
    auto c = req(e, Method::Linear, {"m1.safetensors"}, "o.safetensors");
    c.chunk_elements = 1024;
    c.token = cs.token();
    int calls = 0;
    c.progress = [&](double, const std::string&) {
        if (++calls == 1)
            cs.request_cancel();
    };
    auto rep = run_merge(c, e.guard);
    ASSERT_FALSE(rep.ok());
    EXPECT_EQ(rep.error().code, ErrorCode::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors")));
    EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors.partial")));
}

TEST(MergeFiles, ManifestRecordsTheOutputHashAndProgressIsReported) {
    Env e;
    write_three(e);
    auto r = req(e, Method::Linear, {"m1.safetensors", "m2.safetensors"}, "o.safetensors");
    std::vector<double> prog;
    r.progress = [&](double f, const std::string&) { prog.push_back(f); };
    auto rep = run_merge(r, e.guard);
    ASSERT_TRUE(rep.ok());
    ASSERT_FALSE(prog.empty());
    const std::string bytes = test::read_file(e.p("o.safetensors"));
    const std::string want = storage::to_hex(storage::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())));
    EXPECT_EQ(rep.value().output_sha256, want);
    auto j = nlohmann::json::parse(test::read_file(rep.value().manifest));
    EXPECT_EQ(j["output"]["sha256"], want);
    EXPECT_EQ(j["method"], "linear");
    EXPECT_EQ(j["inputs"].size(), 3u);
    EXPECT_EQ(j["inputs"][0]["role"], "base");
    // The manifest carries file names only, never directories.
    EXPECT_EQ(j["inputs"][1]["file"], "m1.safetensors");
    EXPECT_FALSE(std::filesystem::exists(e.p("o.safetensors.partial")));
}
