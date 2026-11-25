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
