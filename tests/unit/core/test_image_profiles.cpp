#include <gtest/gtest.h>

#include "hcs/image_profiles.hpp"

using namespace hcs;

TEST(ImageProfiles, Sd15DefaultsAre512x512TwentyStepsEulerA) {
    const ImageProfile* p = image_profile("sd15");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->default_width, 512u);
    EXPECT_EQ(p->default_height, 512u);
    EXPECT_EQ(p->default_steps, 20u);
    EXPECT_FLOAT_EQ(p->default_cfg, 7.0f);
    EXPECT_EQ(p->sampler, "euler_a");
}

TEST(ImageProfiles, Flux3KeepsTheMeasuredCfgOfOne) {
    const ImageProfile* p = image_profile("flux3");
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->default_cfg, 1.0f);
    EXPECT_EQ(p->default_steps, 16u);
}

TEST(ImageProfiles, UnknownFamilyHasNoProfileAndNoEnvelope) {
    EXPECT_EQ(image_profile("sdxl"), nullptr);
    EXPECT_TRUE(image_envelope_violation("", 64, 64, 1, false).empty());
    EXPECT_TRUE(image_envelope_violation("sdxl", 64, 64, 1, false).empty());
}

TEST(ImageProfiles, GuessFamilyFromIdOrFileName) {
    EXPECT_EQ(guess_image_family("sd15-q8_0"), "sd15");
    EXPECT_EQ(guess_image_family("D:\\m\\Stable-Diffusion-v1-5.gguf"), "sd15");
    EXPECT_EQ(guess_image_family("flux-3-image-klein-UD-Q4_K_XL"), "flux3");
    EXPECT_EQ(guess_image_family("sdmodel"), "");
    EXPECT_EQ(guess_image_family(""), "");
}

TEST(ImageProfiles, Sd15EnvelopeBoundaries) {
    EXPECT_TRUE(image_envelope_violation("sd15", 512, 512, 20, false).empty());
    EXPECT_TRUE(image_envelope_violation("sd15", 384, 384, 8, false).empty()) << "boundary is inclusive";
    EXPECT_FALSE(image_envelope_violation("sd15", 376, 512, 20, false).empty()) << "short side decides";
    EXPECT_FALSE(image_envelope_violation("sd15", 256, 256, 20, false).empty());
    EXPECT_FALSE(image_envelope_violation("sd15", 512, 512, 7, false).empty());
    const std::string both = image_envelope_violation("sd15", 256, 256, 2, false);
    EXPECT_NE(both.find("minimum short side"), std::string::npos);
    EXPECT_NE(both.find("steps"), std::string::npos);
    EXPECT_NE(both.find("allow_low_quality"), std::string::npos);
}

TEST(ImageProfiles, PresetSkipsTheStepCheckButNotTheSizeCheck) {
    EXPECT_TRUE(image_envelope_violation("flux3", 512, 512, 1, true).empty());
    EXPECT_FALSE(image_envelope_violation("flux3", 256, 256, 1, true).empty());
}

TEST(ImageProfiles, Flux3MeasuredFastPresetIsInsideTheEnvelope) {
    EXPECT_TRUE(image_envelope_violation("flux3", 512, 512, 6, false).empty());
    EXPECT_TRUE(image_envelope_violation("flux3", 512, 512, 4, false).empty());
    EXPECT_FALSE(image_envelope_violation("flux3", 512, 512, 2, false).empty());
}

// ---- wave C / i1: Turbo families, structural classification, per-family presets -------------

TEST(ImageProfiles, GuessTurboFamiliesFromName) {
    EXPECT_EQ(guess_image_family("sd_turbo-f16-q8_0"), "sdturbo");
    EXPECT_EQ(guess_image_family("SD-Turbo"), "sdturbo");
    EXPECT_EQ(guess_image_family("sd_xl_turbo_1.0.q8_0"), "sdxlturbo");
    EXPECT_EQ(guess_image_family("sdxl-turbo"), "sdxlturbo");
    EXPECT_EQ(guess_image_family("sdxl_base_1.0"), "") << "SDXL base has no profile and no name rule";
}

TEST(ImageProfiles, ClassifyByTensorShapeBeatsTheFileName) {
    ImageModelSignature s;
    s.cross_attn_dim = 768;
    EXPECT_EQ(classify_image_family(s, "renamed-model"), "sd15");
    EXPECT_EQ(classify_image_family(s, "sd_turbo"), "sd15") << "768-wide text context is SD1.x whatever the name says";
    s.cross_attn_dim = 1024;
    EXPECT_EQ(classify_image_family(s, "sd_turbo-f16-q8_0"), "sdturbo");
    EXPECT_EQ(classify_image_family(s, "model-x"), "sd2") << "SD2.x shape without a turbo name is plain sd2";
    s.cross_attn_dim = 2048;
    EXPECT_EQ(classify_image_family(s, "sd_xl_turbo_1.0.q8_0"), "sdxlturbo");
    EXPECT_EQ(classify_image_family(s, "x"), "sdxl");
    ImageModelSignature lab;
    lab.has_label_emb = true;
    EXPECT_EQ(classify_image_family(lab, "x"), "sdxl") << "SDXL label embedding without a readable width";
}

TEST(ImageProfiles, ClassifyFallsBackToTheNameWhenTheShapeIsUnknown) {
    ImageModelSignature none;
    EXPECT_EQ(classify_image_family(none, "sd15-q8_0"), "sd15");
    EXPECT_EQ(classify_image_family(none, "flux-3-image-klein"), "flux3");
    EXPECT_EQ(classify_image_family(none, "mystery"), "");
    ImageModelSignature odd;
    odd.cross_attn_dim = 4096;  // not a known text-context width
    EXPECT_EQ(classify_image_family(odd, "mystery"), "");
}

TEST(ImageProfiles, FluxBlocksMapToFlux3ThroughTheSignature) {
    ImageModelSignature f;
    f.has_flux_blocks = true;
    EXPECT_EQ(classify_image_family(f, "weights"), "flux3");
}

TEST(ImageProfiles, TurboProfilesAreCfgOneFewStep) {
    const ImageProfile* t = image_profile("sdturbo");
    ASSERT_NE(t, nullptr);
    EXPECT_FLOAT_EQ(t->default_cfg, 1.0f);
    EXPECT_LE(t->default_steps, 4u);
    const ImageProfile* x = image_profile("sdxlturbo");
    ASSERT_NE(x, nullptr);
    EXPECT_FLOAT_EQ(x->default_cfg, 1.0f);
    EXPECT_LE(x->default_steps, 4u);
}

TEST(ImageProfiles, TurboEnvelopeRejectsManyStepsAndForeignSizes) {
    EXPECT_TRUE(image_envelope_violation("sdturbo", 512, 512, 1, false).empty());
    EXPECT_TRUE(image_envelope_violation("sdturbo", 512, 512, 4, false).empty());
    const std::string many = image_envelope_violation("sdturbo", 512, 512, 20, false);
    EXPECT_NE(many.find("above"), std::string::npos) << many;
    EXPECT_NE(many.find("allow_low_quality"), std::string::npos);
    EXPECT_FALSE(image_envelope_violation("sdturbo", 768, 768, 2, false).empty()) << "SD-Turbo is 512 only";
    EXPECT_FALSE(image_envelope_violation("sdturbo", 256, 256, 2, false).empty());
    EXPECT_TRUE(image_envelope_violation("sdxlturbo", 512, 512, 2, false).empty());
    EXPECT_TRUE(image_envelope_violation("sdxlturbo", 768, 768, 2, false).empty());
    EXPECT_FALSE(image_envelope_violation("sdxlturbo", 1024, 1024, 2, false).empty());
    EXPECT_FALSE(image_envelope_violation("sdxlturbo", 384, 384, 2, false).empty());
    EXPECT_TRUE(image_envelope_violation("sd15", 512, 512, 60, false).empty()) << "families without a cap stay open";
}

TEST(ImageProfiles, PresetsComeFromTheProfile) {
    const auto f = image_preset("flux3", "fast");
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->steps, 6u);
    EXPECT_FLOAT_EQ(f->cfg, 1.0f);
    const auto q = image_preset("flux3", "quality");
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->steps, 16u);
    EXPECT_TRUE(image_preset("sd15", "fast").has_value());
    EXPECT_TRUE(image_preset("sd15", "quality").has_value());
    EXPECT_TRUE(image_preset("sdturbo", "fast").has_value());
    EXPECT_TRUE(image_preset("sdxlturbo", "quality").has_value());
    EXPECT_FALSE(image_preset("sd15", "turbo").has_value());
    EXPECT_FALSE(image_preset("sdxl", "fast").has_value()) << "no profile, no preset";
    EXPECT_FALSE(image_preset("", "fast").has_value());
}

TEST(ImageProfiles, PresetPointsLieInsideTheirOwnEnvelope) {
    for (const char* fam : {"sd15", "flux3", "sdturbo", "sdxlturbo"}) {
        const ImageProfile* p = image_profile(fam);
        ASSERT_NE(p, nullptr) << fam;
        for (const char* name : {"fast", "quality"}) {
            const auto pt = image_preset(fam, name);
            ASSERT_TRUE(pt.has_value()) << fam << " " << name;
            EXPECT_TRUE(image_envelope_violation(fam, p->default_width, p->default_height, pt->steps, false).empty())
                << fam << " " << name;
        }
    }
}

TEST(ImageProfiles, BaseFamilyMapsTurboVariantsForLoraCompatibility) {
    EXPECT_EQ(image_base_family("sdturbo"), "sd2");
    EXPECT_EQ(image_base_family("sdxlturbo"), "sdxl");
    EXPECT_EQ(image_base_family("sd15"), "sd15");
    EXPECT_EQ(image_base_family("flux3"), "flux3");
    EXPECT_EQ(image_base_family(""), "");
}

TEST(ImageProfiles, SamplerAndSchedulerAllowLists) {
    for (const char* s : {"euler", "euler_a", "heun", "dpm++2m", "lcm", "ddim_trailing"})
        EXPECT_TRUE(is_known_image_sampler(s)) << s;
    for (const char* s : {"", "LCM", "euler_a;rm", "../x", "dpm++2m ", "not_a_sampler"})
        EXPECT_FALSE(is_known_image_sampler(s)) << "'" << s << "'";
    for (const char* s : {"discrete", "karras", "exponential", "simple", "sgm_uniform", "lcm"})
        EXPECT_TRUE(is_known_image_scheduler(s)) << s;
    for (const char* s : {"", "Karras", "normal", "x"})
        EXPECT_FALSE(is_known_image_scheduler(s)) << "'" << s << "'";
}
