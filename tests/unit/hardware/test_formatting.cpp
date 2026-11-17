#include <gtest/gtest.h>

#include "hw.hpp"

using namespace hcs::hardware;

TEST(HardwareFormat, Bytes) {
    EXPECT_EQ(format_bytes(0), "0 B");
    EXPECT_EQ(format_bytes(1023), "1023 B");
    EXPECT_EQ(format_bytes(1024), "1.00 KiB");
    EXPECT_EQ(format_bytes(3ull << 30), "3.00 GiB");
    EXPECT_EQ(format_bytes(1536ull << 20), "1.50 GiB");
}

TEST(HardwareFormat, MibS) {
    EXPECT_EQ(format_mib_s(123.456), "123.5 MiB/s");
}

TEST(HardwareFormat, BusAndDriveTypes) {
    EXPECT_EQ(bus_type_name(7), "usb");
    EXPECT_EQ(bus_type_name(0x11), "nvme");
    EXPECT_EQ(bus_type_name(0xB), "sata");
    EXPECT_EQ(bus_type_name(0xFFFF), "unknown");
    EXPECT_EQ(drive_type_name(3), "fixed");
    EXPECT_EQ(drive_type_name(2), "removable");
    EXPECT_EQ(drive_type_name(99), "unknown");
}

TEST(HardwareFormat, TimestampLooksLikeUtcIso) {
    const std::string t = utc_timestamp_now();
    ASSERT_EQ(t.size(), 20u);
    EXPECT_EQ(t[4], '-');
    EXPECT_EQ(t[10], 'T');
    EXPECT_EQ(t.back(), 'Z');
}

TEST(HardwareFormat, RenderTextHandlesEmptyReportWithUnknown) {
    const std::string text = render_text(json::object());
    EXPECT_NE(text.find("unknown"), std::string::npos);
    EXPECT_NE(text.find("Vulkan"), std::string::npos);
}

TEST(HardwareFormat, RenderTextShowsSyntheticValues) {
    json r;
    r["schema_version"] = 1;
    r["cpu"] = {{"name", "Test CPU"}, {"physical_cores", 8}, {"logical_processors", 16}};
    r["memory"] = {{"total_physical_bytes", 4ull << 30}};
    r["vulkan"] = {{"status", "skipped"}, {"reason", "--no-vulkan"}, {"devices", json::array()}};
    const std::string text = render_text(r);
    EXPECT_NE(text.find("Test CPU"), std::string::npos);
    EXPECT_NE(text.find("4.00 GiB"), std::string::npos);
    EXPECT_NE(text.find("skipped"), std::string::npos);
    EXPECT_NE(text.find("8 / 16"), std::string::npos);
}
