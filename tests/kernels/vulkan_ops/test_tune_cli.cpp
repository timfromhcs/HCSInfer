// `HCSInfer tune` command-line contract that does not depend on timing: argument validation (no GPU needed),
// `--show` and `--reset` (autotuner reset / rebenchmark, wave C / k1) against a seeded profile in a temp
// state dir. A test that needs a Vulkan device reports GTEST_SKIP("... NOT RUN") when there is none.
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <sstream>

#include "hcs/hardware/caps.hpp"
#include "hcs/hardware/kernel_profile.hpp"
#include "hcs/kernels/vk_tune.hpp"
#include "temp_dir.hpp"

using namespace hcs;
namespace hw = hcs::hardware;
namespace vk = hcs::kernels::vulkan;

namespace {

struct TuneRun {
    int rc = -1;
    std::string out, err;
};

TuneRun tune(std::vector<std::string> args, const std::filesystem::path& state) {
    std::ostringstream o, e;
    TuneRun r;
    r.rc = vk::run_tune(std::move(args), state, o, e);
    r.out = o.str();
    r.err = e.str();
    return r;
}

hw::ProfileEntry entry(const std::string& device_key, const std::string& kernel, const std::string& bucket) {
    hw::ProfileEntry e;
    e.device_key = device_key;
    e.kernel_id = kernel;
    e.kernel_hash = "0123456789abcdef";
    e.shape_bucket = bucket;
    e.correct = true;
    e.identical_runs = e.total_runs = 3;
    e.samples = 15;
    e.wall_ms_median = 1.0;
    e.baseline_name = "cpu_mt_autovec";
    e.baseline_ms = 4.0;
    return e;
}

}  // namespace

TEST(TuneCli, HelpMentionsResetAndTheUpstreamBaseline) {
    hcs::test::TempDir d;
    const auto r = tune({"--help"}, d.path());
    EXPECT_EQ(r.rc, 0);
    EXPECT_NE(r.out.find("--reset"), std::string::npos);
    EXPECT_NE(r.out.find("--no-upstream"), std::string::npos);
    EXPECT_NE(r.out.find("llm"), std::string::npos);
}

TEST(TuneCli, InvalidArgumentsExitWithTheInvalidArgumentCodeBeforeAnyGpuWork) {
    hcs::test::TempDir d;
    for (const auto& bad : std::vector<std::vector<std::string>>{{"--suite", "bogus"},
                                                                 {"--samples", "2"},
                                                                 {"--samples", "abc"},
                                                                 {"--kernel", "no_such_kernel"},
                                                                 {"--device"},
                                                                 {"--nonsense"}}) {
        const auto r = tune(bad, d.path());
        EXPECT_EQ(r.rc, exit_code(ErrorCode::InvalidArgument)) << bad.front() << ": " << r.err;
        EXPECT_NE(r.err.find("invalid_argument"), std::string::npos);
    }
}

TEST(TuneCli, ResetDropsOnlyTheSelectedDeviceAndKernelAndShowReflectsIt) {
    std::string st, rs;
    const auto devices = hw::probe_vulkan_caps(&st, &rs);
    if (devices.empty())
        GTEST_SKIP() << "NOT RUN: no Vulkan device (" << st << ")";
    const hw::DeviceCaps& caps = devices.front();
    hcs::test::TempDir d;
    const std::string other_device = "vk:ffff:ffff:drv=x:api=1.0.0";
    hw::KernelProfile p;
    ASSERT_TRUE(p.upsert(entry(caps.device_key(), "gemm_f32_tiled", "m256n1024k256")).ok());
    ASSERT_TRUE(p.upsert(entry(caps.device_key(), "gemm_f32_tiled", "m1n4096k4096")).ok());
    ASSERT_TRUE(p.upsert(entry(caps.device_key(), "softmax_rows", "m32n4096k0")).ok());
    ASSERT_TRUE(p.upsert(entry(other_device, "gemm_f32_tiled", "m256n1024k256")).ok());
    ASSERT_TRUE(hw::save_profile(hw::profile_path(d.path()), p).ok());

    auto count_for = [&](const std::string& key) {
        auto loaded = hw::load_profile(hw::profile_path(d.path()));
        EXPECT_TRUE(loaded.ok());
        std::size_t n = 0;
        for (const auto& e : loaded.value().entries())
            if (e.device_key == key)
                ++n;
        return n;
    };
    ASSERT_EQ(count_for(caps.device_key()), 3u);

    auto r = tune({"--show", "--device", "0"}, d.path());
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("gemm_f32_tiled"), std::string::npos);

    r = tune({"--reset", "--device", "0", "--kernel", "gemm_f32_tiled"}, d.path());
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("removed 2 profile entries"), std::string::npos) << r.out;
    EXPECT_EQ(count_for(caps.device_key()), 1u);  // softmax_rows survived
    EXPECT_EQ(count_for(other_device), 1u);       // another device is never touched

    r = tune({"--reset", "--device", "0"}, d.path());
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("removed 1 profile entries"), std::string::npos) << r.out;
    EXPECT_EQ(count_for(caps.device_key()), 0u);
    EXPECT_EQ(count_for(other_device), 1u);

    r = tune({"--reset", "--device", "0"}, d.path());  // idempotent
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.out.find("removed 0 profile entries"), std::string::npos) << r.out;
}

TEST(TuneCli, ACorruptProfileIsReportedAndNeverReplacedByReset) {
    std::string st, rs;
    if (hw::probe_vulkan_caps(&st, &rs).empty())
        GTEST_SKIP() << "NOT RUN: no Vulkan device (" << st << ")";
    hcs::test::TempDir d;
    {
        std::ofstream f(hw::profile_path(d.path()), std::ios::binary);
        f << "this is not json";
    }
    const auto r = tune({"--reset", "--device", "0"}, d.path());
    EXPECT_NE(r.rc, 0);
    EXPECT_NE(r.err.find("unreadable"), std::string::npos) << r.err;
    std::ifstream in(hw::profile_path(d.path()), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, "this is not json");  // untouched
}
