// Live smoke tests: they query the machine the tests run on. Assertions are about structure and
// invariants (never about a specific machine); hardware-dependent checks skip with a reason.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "hcs/hardware/doctor.hpp"
#include "temp_dir.hpp"

using nlohmann::json;
using namespace hcs::hardware;

namespace {
json report(const DoctorOptions& o) {
    bool ok = false;
    const std::string text = collect_report_json(o, &ok);
    EXPECT_TRUE(ok) << text;
    return json::parse(text);
}
}  // namespace

TEST(DoctorLive, ReportStructureNoVulkan) {
    DoctorOptions o;
    o.no_vulkan = true;
    const json r = report(o);
    EXPECT_EQ(r["schema_version"], 1);
    EXPECT_EQ(r["vulkan"]["status"], "skipped");
    EXPECT_TRUE(r["vulkan"]["devices"].empty());
    EXPECT_EQ(r["summary"]["vulkan_status"], "skipped");
    EXPECT_TRUE(r["cpu"]["name"].is_string());
    ASSERT_TRUE(r["cpu"]["logical_processors"].is_number());
    EXPECT_GE(r["cpu"]["logical_processors"].get<int>(), 1);
    ASSERT_TRUE(r["memory"]["total_physical_bytes"].is_number());
    EXPECT_GT(r["memory"]["total_physical_bytes"].get<std::uint64_t>(), 0u);
    EXPECT_EQ(r["memory"]["channel_configuration"], "unknown");  // never guessed
    EXPECT_EQ(r["memory"]["bios_uma_reservation"], "unknown");
    EXPECT_TRUE(r["summary"].contains("ram_bytes"));
    EXPECT_TRUE(r["summary"].contains("os_build"));
}

TEST(DoctorLive, CpuFeaturesPresent) {
    DoctorOptions o;
    o.no_vulkan = true;
    const json r = report(o);
    ASSERT_TRUE(r["cpu"]["features"].contains("avx2"));
    ASSERT_TRUE(r["cpu"]["features"].contains("fma3"));
}

TEST(DoctorLive, VulkanReportWhenAvailable) {
    const json r = report(DoctorOptions{});
    const std::string status = r["vulkan"]["status"];
    if (status != "ok") GTEST_SKIP() << "BLOCKED: Vulkan not usable here (status=" << status << ")";
    ASSERT_FALSE(r["vulkan"]["devices"].empty());
    const json& d = r["vulkan"]["devices"][0];
    EXPECT_TRUE(d["name"].is_string());
    EXPECT_TRUE(d["vendor_id"].is_string());
    EXPECT_TRUE(d.contains("queue_families"));
    EXPECT_TRUE(d.contains("memory"));
    EXPECT_NE(r["summary"]["gpu"], "unknown");
}

TEST(DoctorLive, ReadBenchmarkOnTempFile) {
    hcs::test::TempDir t;
    const auto f = t / "bench.bin";
    {
        std::ofstream out(f, std::ios::binary);
        std::string chunk(1 << 20, 'x');
        for (int i = 0; i < 3; ++i) out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    DoctorOptions o;
    o.no_vulkan = true;
    o.read_file = f.string();
    o.read_buffer_bytes = 64u << 10;
    o.read_repeats = 2;
    o.read_max_bytes = 2u << 20;  // bounded: only 2 of 3 MiB
    const json r = report(o);
    const json& b = r["read_benchmark"];
    EXPECT_EQ(b["status"], "ok") << b.dump();
    EXPECT_EQ(b["cold_or_warm"], "unknown");
    ASSERT_EQ(b["samples"].size(), 2u);
    EXPECT_EQ(b["samples"][0]["bytes_read"].get<std::uint64_t>(), 2u << 20);
    EXPECT_EQ(b["file_size_bytes"].get<std::uint64_t>(), 3u << 20);
    EXPECT_TRUE(b["mib_s_median"].is_number());
    // The benchmark must not modify the file.
    EXPECT_EQ(std::filesystem::file_size(f), 3u << 20);
    EXPECT_TRUE(r["storage"]["requested_volume"].contains("filesystem"));
}

TEST(DoctorLive, ReadBenchmarkMissingFileIsErrorNotCrash) {
    DoctorOptions o;
    o.no_vulkan = true;
    o.read_file = "Z:\\definitely\\does\\not\\exist.bin";
    const json r = report(o);
    EXPECT_EQ(r["read_benchmark"]["status"], "error");
    EXPECT_EQ(r["read_benchmark"]["mib_s_median"], "unknown");
}

TEST(DoctorLive, RejectsInvalidOptions) {
    DoctorOptions o;
    o.read_buffer_bytes = 10;
    bool ok = true;
    collect_report_json(o, &ok);
    EXPECT_FALSE(ok);
    o = DoctorOptions{};
    o.read_repeats = 99;
    collect_report_json(o, &ok);
    EXPECT_FALSE(ok);
    std::ostringstream out, err;
    o.json = true;
    EXPECT_EQ(run_doctor(o, out, err), 2);
    EXPECT_TRUE(out.str().empty());
}

TEST(DoctorLive, TextAndJsonRunnersWork) {
    DoctorOptions o;
    o.no_vulkan = true;
    std::ostringstream text, js, err;
    EXPECT_EQ(run_doctor(o, text, err), 0) << err.str();
    EXPECT_NE(text.str().find("HCSInfer doctor"), std::string::npos);
    o.json = true;
    EXPECT_EQ(run_doctor(o, js, err), 0);
    EXPECT_FALSE(json::parse(js.str(), nullptr, false).is_discarded());
}
