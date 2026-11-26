#include <gtest/gtest.h>

#include "hcs/engines/diffusion_runtime.hpp"

using hcs::diffusion_runtime_options;

TEST(DiffusionRuntime, GpuPinsKernelChoiceSoNoTimingProbeRuns) {
    const auto o = diffusion_runtime_options(true);
    EXPECT_EQ(o.flash_attn_mode, "off");
    EXPECT_EQ(o.conv_direct_mode, "off");
}

TEST(DiffusionRuntime, CpuKeepsUpstreamDefaults) {
    const auto o = diffusion_runtime_options(false);
    EXPECT_TRUE(o.flash_attn_mode.empty());
    EXPECT_TRUE(o.conv_direct_mode.empty());
}
