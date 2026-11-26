// Image LoRA library: manifest parsing, scanner, compatibility and request resolution. Synthetic files only.
#include <gtest/gtest.h>

#include "hcs/lora.hpp"
#include "hcs/safetensors.hpp"
#include "hcs/storage/sha256.hpp"
#include "safetensors_builder.hpp"
#include "temp_dir.hpp"

using namespace hcs;
using hcs::test::build_safetensors;
using hcs::test::f32_tensor;

namespace {

std::string lora_file(const std::vector<std::string>& names) {
    std::vector<test::StBuildTensor> ts;
    for (const auto& n : names) ts.push_back(f32_tensor(n, {2, 2}, {1, 2, 3, 4}));
    return build_safetensors(ts);
}

const std::vector<std::string> kSd1Names = {"lora_unet_down_blocks_0.lora_down.weight",
                                            "lora_unet_down_blocks_0.lora_up.weight",
                                            "lora_te_text_model_encoder.lora_down.weight"};
const std::vector<std::string> kSdxlNames = {"lora_unet_input_blocks_4.lora_down.weight",
                                             "lora_te1_text_model.lora_down.weight",
                                             "lora_te2_text_model.lora_down.weight"};
const std::vector<std::string> kFluxNames = {"lora_unet_double_blocks_0_img_attn_qkv.lora_down.weight"};

std::string manifest(const std::string& entries) {
    return R"({"schema_version":1,"loras":[)" + entries + "]}";
}

struct Fixture {
    test::TempDir td;
    PathGuard guard;
    Fixture() { EXPECT_TRUE(guard.add_root(td.path()).ok()); }
    std::filesystem::path dir() const { return td.path() / "lora" / "image"; }
};

}  // namespace

TEST(LoraFamilies, KnownGroupsAndExactCompat) {
    EXPECT_TRUE(is_known_lora_family("sd15"));
    EXPECT_TRUE(is_known_lora_family("flux3"));
    EXPECT_FALSE(is_known_lora_family("flux"));
    EXPECT_FALSE(is_known_lora_family(""));
    EXPECT_EQ(lora_family_group("flux1"), "flux");
    EXPECT_EQ(lora_family_group("sd2"), "sd1");
    EXPECT_EQ(lora_compatibility("sd15", "sd15"), LoraCompat::Compatible);
    EXPECT_EQ(lora_compatibility("sd15", "flux3"), LoraCompat::Incompatible);
    // Same tensor-naming group but a different architecture generation is still incompatible.
    EXPECT_EQ(lora_compatibility("flux1", "flux3"), LoraCompat::Incompatible);
    EXPECT_EQ(lora_compatibility("sd15", ""), LoraCompat::Unknown);
}

TEST(LoraSniff, DistinguishesGroups) {
    EXPECT_EQ(sniff_lora_group(kSd1Names), "sd1");
    EXPECT_EQ(sniff_lora_group(kSdxlNames), "sdxl");
    EXPECT_EQ(sniff_lora_group(kFluxNames), "flux");
    EXPECT_EQ(sniff_lora_group({"transformer.joint_blocks.0.lora_A.weight"}), "sd3");
    EXPECT_EQ(sniff_lora_group({"some.other.weight"}), "");
}

TEST(LoraWeight, Bounds) {
    EXPECT_TRUE(validate_lora_weight(1.0f).ok());
    EXPECT_TRUE(validate_lora_weight(-2.0f).ok());
    EXPECT_FALSE(validate_lora_weight(0.0f).ok());
    EXPECT_FALSE(validate_lora_weight(2.01f).ok());
    EXPECT_FALSE(validate_lora_weight(std::numeric_limits<float>::quiet_NaN()).ok());
    EXPECT_FALSE(validate_lora_weight(std::numeric_limits<float>::infinity()).ok());
}

TEST(LoraManifest, ParsesFullEntry) {
    auto r = parse_lora_manifest(manifest(
        R"({"id":"detail-sd15","file":"detail.safetensors","name":"Detail","base_family":"sd15",
            "trigger_words":["add_detail"],"recommended_weight":0.7,"licence":"CreativeML Open RAIL-M",
            "source":"https://example.invalid/x","sha256":"AA00000000000000000000000000000000000000000000000000000000000000"})"));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().entries.size(), 1u);
    const auto& e = r.value().entries[0];
    EXPECT_EQ(e.status, "ready");
    EXPECT_EQ(e.id, "detail-sd15");
    EXPECT_EQ(e.base_family, "sd15");
    EXPECT_FLOAT_EQ(e.recommended_weight, 0.7f);
    ASSERT_EQ(e.trigger_words.size(), 1u);
    EXPECT_EQ(e.sha256[0], 'a');  // normalised to lower case
}

TEST(LoraManifest, RejectsStructuralErrorsAndMarksBadEntries) {
    EXPECT_FALSE(parse_lora_manifest("not json").ok());
    EXPECT_FALSE(parse_lora_manifest(R"({"schema_version":2,"loras":[]})").ok());
    EXPECT_FALSE(parse_lora_manifest(R"({"schema_version":1})").ok());
    auto r = parse_lora_manifest(manifest(
        R"({"id":"a","file":"../evil.safetensors","base_family":"sd15","licence":"x"},
            {"id":"b","file":"b.safetensors","base_family":"flux","licence":"x"},
            {"id":"c","file":"c.safetensors","base_family":"sd15"},
            {"id":"d","file":"d.safetensors","base_family":"sd15","licence":"x","recommended_weight":9},
            {"id":"e","file":"e.safetensors","base_family":"sd15","licence":"x"},
            {"id":"e","file":"e2.safetensors","base_family":"sd15","licence":"x"},
            {"id":"bad id!","file":"f.safetensors","base_family":"sd15","licence":"x"},
            {"id":"g","file":"g.safetensors","base_family":"sd15","licence":"x","sha256":"zz"},
            42)"));
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().entries.size(), 9u);
    for (std::size_t i = 0; i < 9; ++i) {
        if (i == 4)
            EXPECT_EQ(r.value().entries[i].status, "ready");
        else
            EXPECT_EQ(r.value().entries[i].status, "invalid") << "entry " << i;
    }
    EXPECT_NE(r.value().entries[5].problems.front().find("duplicate"), std::string::npos);
}

TEST(LoraScan, MissingFolderIsEmptyNotAnError) {
    Fixture f;
    LoraLibrary lib;
    ASSERT_TRUE(lib.scan(f.guard, f.dir()).ok());
    EXPECT_TRUE(lib.entries().empty());
    EXPECT_FALSE(lib.scan_note().empty());
}

TEST(LoraScan, FolderOutsideApprovedRootIsRejected) {
    Fixture f;
    test::TempDir other;
    LoraLibrary lib;
    auto r = lib.scan(f.guard, other.path());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::SecurityViolation);
}

TEST(LoraScan, ClassifiesReadyMissingMismatchedAndUnmanifested) {
    Fixture f;
    test::write_file(f.dir() / "good.safetensors", lora_file(kSd1Names));
    test::write_file(f.dir() / "flux.safetensors", lora_file(kFluxNames));
    test::write_file(f.dir() / "extra.safetensors", lora_file(kSd1Names));
    test::write_file(f.dir() / "notlora.safetensors", build_safetensors({f32_tensor("w", {1}, {1})}));
    test::write_file(f.dir() / "broken.safetensors", std::string("\x05\0\0\0\0\0\0\0xx", 10));
    test::write_file(
        f.dir() / "manifest.json",
        manifest(
            R"({"id":"good","file":"good.safetensors","base_family":"sd15","licence":"MIT","recommended_weight":0.8},
                {"id":"liar","file":"flux.safetensors","base_family":"sd15","licence":"MIT"},
                {"id":"gone","file":"gone.safetensors","base_family":"sd15","licence":"MIT"},
                {"id":"nl","file":"notlora.safetensors","base_family":"sd15","licence":"MIT"},
                {"id":"br","file":"broken.safetensors","base_family":"sd15","licence":"MIT"})"));
    LoraLibrary lib;
    ASSERT_TRUE(lib.scan(f.guard, f.dir()).ok());
    ASSERT_NE(lib.find("good"), nullptr);
    EXPECT_EQ(lib.find("good")->status, "ready");
    EXPECT_EQ(lib.find("good")->sniffed_group, "sd1");
    EXPECT_EQ(lib.find("good")->tensor_count, 3u);
    EXPECT_FALSE(lib.find("good")->path.empty());
    EXPECT_EQ(lib.find("liar")->status, "invalid");
    EXPECT_NE(lib.find("liar")->problems.front().find("flux"), std::string::npos);
    EXPECT_EQ(lib.find("gone")->status, "missing");
    EXPECT_EQ(lib.find("nl")->status, "invalid");
    EXPECT_EQ(lib.find("br")->status, "invalid");
    ASSERT_NE(lib.find("extra.safetensors"), nullptr);
    EXPECT_EQ(lib.find("extra.safetensors")->status, "unmanifested");
    EXPECT_FALSE(lib.find("extra.safetensors")->usable());
}

TEST(LoraScan, HashVerification) {
    Fixture f;
    const std::string bytes = lora_file(kSd1Names);
    test::write_file(f.dir() / "a.safetensors", bytes);
    const std::string good = storage::to_hex(storage::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())));
    const std::string bad(64, '0');
    test::write_file(
        f.dir() / "manifest.json",
        manifest(R"({"id":"a","file":"a.safetensors","base_family":"sd15","licence":"x","sha256":")" + good +
                 R"("},{"id":"b","file":"a.safetensors","base_family":"sd15","licence":"x","sha256":")" +
                 bad + R"("})"));
    LoraLibrary lib;
    ASSERT_TRUE(lib.scan(f.guard, f.dir(), {.verify_hashes = true}).ok());
    EXPECT_EQ(lib.find("a")->sha256_status, "ok");
    EXPECT_EQ(lib.find("a")->status, "ready");
    EXPECT_EQ(lib.find("b")->sha256_status, "mismatch");
    EXPECT_EQ(lib.find("b")->status, "invalid");
}

TEST(LoraScan, DeclaredSha256NotCheckedByDefault) {
    Fixture f;
    test::write_file(f.dir() / "a.safetensors", lora_file(kSd1Names));
    test::write_file(
        f.dir() / "manifest.json",
        manifest(R"({"id":"a","file":"a.safetensors","base_family":"sd15","licence":"x","sha256":")" +
                 std::string(64, '1') + R"("})"));
    LoraLibrary lib;
    ASSERT_TRUE(lib.scan(f.guard, f.dir()).ok());
    EXPECT_EQ(lib.find("a")->sha256_status, "unverified");
    EXPECT_TRUE(lib.find("a")->usable());
}

TEST(LoraResolve, FailsClosedWithStructuredErrors) {
    Fixture f;
    test::write_file(f.dir() / "sd.safetensors", lora_file(kSd1Names));
    test::write_file(f.dir() / "fx.safetensors", lora_file(kFluxNames));
    test::write_file(
        f.dir() / "manifest.json",
        manifest(
            R"({"id":"sd","file":"sd.safetensors","base_family":"sd15","licence":"x","recommended_weight":0.6},
                    {"id":"fx","file":"fx.safetensors","base_family":"flux3","licence":"x"},
                    {"id":"gone","file":"gone.safetensors","base_family":"sd15","licence":"x"})"));
    LoraLibrary lib;
    ASSERT_TRUE(lib.scan(f.guard, f.dir()).ok());

    auto ok = lib.resolve({{"sd", 0.0f, false}}, "sd15");
    ASSERT_TRUE(ok.ok());
    ASSERT_EQ(ok.value().size(), 1u);
    EXPECT_FLOAT_EQ(ok.value()[0].weight, 0.6f);  // manifest recommendation when no weight is given
    EXPECT_EQ(ok.value()[0].base_family, "sd15");

    auto w = lib.resolve({{"sd", 1.5f, true}}, "sd15");
    ASSERT_TRUE(w.ok());
    EXPECT_FLOAT_EQ(w.value()[0].weight, 1.5f);

    auto wrong = lib.resolve({{"sd", 1.0f, true}}, "flux3");
    ASSERT_FALSE(wrong.ok());
    EXPECT_EQ(wrong.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(wrong.error().message.find("lora_incompatible"), std::string::npos);

    auto wrong2 = lib.resolve({{"fx", 1.0f, true}}, "sd15");
    ASSERT_FALSE(wrong2.ok());
    EXPECT_NE(wrong2.error().message.find("lora_incompatible"), std::string::npos);

    auto unknown_ok = lib.resolve({{"sd", 1.0f, true}}, "");
    EXPECT_TRUE(unknown_ok.ok());
    auto unknown_strict = lib.resolve({{"sd", 1.0f, true}}, "", false);
    ASSERT_FALSE(unknown_strict.ok());
    EXPECT_NE(unknown_strict.error().message.find("lora_incompatible"), std::string::npos);

    EXPECT_EQ(lib.resolve({{"nope", 1.0f, true}}, "sd15").error().code, ErrorCode::NotFound);
    EXPECT_EQ(lib.resolve({{"gone", 1.0f, true}}, "sd15").error().code, ErrorCode::ModelError);
    EXPECT_EQ(lib.resolve({{"sd", 5.0f, true}}, "sd15").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(lib.resolve({{"sd", 1.0f, true}, {"sd", 1.0f, true}}, "sd15").error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(lib.resolve({{"../x", 1.0f, true}}, "sd15").error().code, ErrorCode::InvalidArgument);
    std::vector<LoraLibrary::Ref> five(5, {"sd", 1.0f, true});
    EXPECT_EQ(lib.resolve(five, "sd15").error().code, ErrorCode::InvalidArgument);
    auto none = lib.resolve({}, "sd15");
    ASSERT_TRUE(none.ok());
    EXPECT_TRUE(none.value().empty());
}
