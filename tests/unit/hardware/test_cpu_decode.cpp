#include <gtest/gtest.h>

#include "hw.hpp"

using namespace hcs::hardware;

namespace {
CpuRaw zen3_like() {
    CpuRaw r;
    r.vendor = "AuthenticAMD";
    r.brand = "Synthetic CPU";
    r.max_leaf = 0x10;
    r.l1.eax = 0x00A40F41;  // family 0xF + ext 0x0A => 0x19, ext model 4, model 4, stepping 1
    r.l1.ecx = (1u << 12) | (1u << 20) | (1u << 23) | (1u << 25) | (1u << 27) | (1u << 28) | (1u << 29);
    r.l7.eax = 0;
    r.l7.ebx = (1u << 3) | (1u << 5) | (1u << 8) | (1u << 29);
    r.has_xgetbv = true;
    r.xcr0 = 0x7;  // x87 + SSE + AVX state enabled, no AVX-512 state
    return r;
}
}  // namespace

TEST(CpuDecode, IdentityFamilyModelStepping) {
    const json id = decode_cpu_identity(zen3_like());
    EXPECT_EQ(id["vendor"], "AuthenticAMD");
    EXPECT_EQ(id["family"], 0x19);
    EXPECT_EQ(id["model"], 0x44);  // ext model 4 << 4 | model 4
    EXPECT_EQ(id["stepping"], 1);
}

TEST(CpuDecode, FeaturesRequireOsSupport) {
    CpuRaw r = zen3_like();
    json f = decode_cpu_features(r);
    EXPECT_TRUE(f["avx2"].get<bool>());
    EXPECT_TRUE(f["fma3"].get<bool>());
    EXPECT_TRUE(f["bmi2"].get<bool>());
    EXPECT_FALSE(f["avx512f"].get<bool>());

    r.xcr0 = 0x1;  // OS did not enable YMM state: AVX2/FMA must NOT be reported usable
    f = decode_cpu_features(r);
    EXPECT_FALSE(f["avx2"].get<bool>());
    EXPECT_FALSE(f["fma3"].get<bool>());
    EXPECT_FALSE(f["os_ymm_state_enabled"].get<bool>());
}

TEST(CpuDecode, Avx512NeedsZmmState) {
    CpuRaw r = zen3_like();
    r.l7.ebx |= (1u << 16);
    EXPECT_FALSE(decode_cpu_features(r)["avx512f"].get<bool>());
    r.xcr0 = 0xE7;
    EXPECT_TRUE(decode_cpu_features(r)["avx512f"].get<bool>());
}

TEST(CpuDecode, NoCpuidReportsUnknownNotFalse) {
    CpuRaw r;  // max_leaf == 0: nothing known
    const json f = decode_cpu_features(r);
    EXPECT_EQ(f["avx2"], "unknown");
    EXPECT_EQ(decode_cpu_identity(r)["name"], "unknown");
    EXPECT_EQ(decode_cpu_identity(r)["family"], "unknown");
}
