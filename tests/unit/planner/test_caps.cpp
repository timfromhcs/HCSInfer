#include <gtest/gtest.h>

#include "planner_fixtures.hpp"

using namespace hcs::test;

TEST(Caps, Decodes680mProbeRecord) {
    const DeviceCaps c = caps_680m();
    EXPECT_EQ(c.name, "AMD Radeon(TM) Graphics");
    EXPECT_EQ(c.vendor_id, 0x1002u);
    EXPECT_EQ(c.device_id, 0x1681u);
    EXPECT_EQ(c.subgroup_size.value_or(0), 64u);
    EXPECT_EQ(c.max_shared_memory.value_or(0), 32768u);
    EXPECT_EQ(c.max_workgroup_invocations.value_or(0), 1024u);
    EXPECT_EQ(c.max_storage_range.value_or(0), 4294967295ull);
    EXPECT_TRUE(is_yes(c.shader_float16));
    EXPECT_TRUE(is_yes(c.integer_dot_product));
    EXPECT_TRUE(is_yes(c.int8_dot_accelerated));
    EXPECT_TRUE(is_yes(c.cooperative_matrix));
    EXPECT_EQ(c.float8_ext, Cap::No);  // reported absent, not unknown
    EXPECT_TRUE(is_yes(c.timestamps));
    EXPECT_TRUE(c.has_coopmat_f16_f32_16x16x16_subgroup());
    ASSERT_EQ(c.heaps.size(), 2u);
    EXPECT_TRUE(c.heaps[0].device_local);
    EXPECT_EQ(c.heaps[0].budget_bytes.value_or(0), 8000000000ull);
    EXPECT_FALSE(c.heaps[1].budget_bytes.has_value());  // "unknown" stays unknown
    EXPECT_EQ(c.device_key(), "vk:1002:1681:drv=0x8008A000:api=1.4.315");
    ASSERT_TRUE(c.compute_queue_family().has_value());
}

TEST(Caps, OldDeviceKeepsEverythingUnknownAndDoesNotInventSupport) {
    const DeviceCaps c = caps_from_probe(old_vulkan_device_probe());
    EXPECT_EQ(c.shader_float16, Cap::Unknown);
    EXPECT_EQ(c.integer_dot_product, Cap::Unknown);
    EXPECT_EQ(c.int8_dot_accelerated, Cap::Unknown);
    EXPECT_EQ(c.cooperative_matrix, Cap::No);  // the extension is positively absent
    EXPECT_FALSE(c.subgroup_size.has_value());
    EXPECT_FALSE(c.max_shared_memory.has_value());
    EXPECT_FALSE(c.has_coopmat_f16_f32_16x16x16_subgroup());
    EXPECT_EQ(c.timestamps, Cap::No);  // queue family present, zero valid bits
    // dtype table: f32 always; i8 needs the dot-product feature which is unknown
    EXPECT_EQ(c.dtype_compute(DType::F32), Support::Native);
    EXPECT_EQ(c.dtype_compute(DType::I8), Support::Unsupported);
    EXPECT_EQ(c.dtype_compute(DType::F16), Support::Emulated);  // packed halves need no feature
}

TEST(Caps, NoComputeQueueMeansNothingComputes) {
    json p = radeon_680m_probe();
    p["queue_families"] = {{{"index", 0}, {"queue_count", 1}, {"flags", {"transfer"}}, {"timestamp_valid_bits", 0}}};
    const DeviceCaps c = caps_from_probe(p);
    EXPECT_FALSE(c.compute_queue_family().has_value());
    for (DType d : {DType::F32, DType::F16, DType::I8})
        EXPECT_FALSE(can_compute(c.dtype_compute(d)));
}

TEST(Caps, CoopMatFeatureNeedsTheExtension) {
    json p = radeon_680m_probe();
    p["relevant_extensions"]["VK_KHR_cooperative_matrix"] = false;
    EXPECT_EQ(caps_from_probe(p).cooperative_matrix, Cap::No);
}

TEST(Caps, SelectDeviceByIndexAliasIdsAndName) {
    const std::vector<DeviceCaps> v = {caps_680m(), caps_from_probe(old_vulkan_device_probe())};
    EXPECT_EQ(select_device(v, "").value(), 0u);
    EXPECT_EQ(select_device(v, "1").value(), 1u);
    EXPECT_FALSE(select_device(v, "2").has_value());
    EXPECT_EQ(select_device(v, "radeon-680m").value(), 0u);
    EXPECT_EQ(select_device(v, "0x10de:0x1234").value(), 1u);
    EXPECT_EQ(select_device(v, "old vulkan").value(), 1u);
    EXPECT_EQ(select_device(v, "AMD").value(), 0u);
    EXPECT_FALSE(select_device(v, "nonexistent").has_value());
    EXPECT_FALSE(select_device({}, "0").has_value());
    // alias must not match a different GPU
    const std::vector<DeviceCaps> only_old = {caps_from_probe(old_vulkan_device_probe())};
    EXPECT_FALSE(select_device(only_old, "radeon-680m").has_value());
}

TEST(Caps, JsonExportUsesUnknownStringsNotGuesses) {
    const json j = caps_to_json(caps_from_probe(old_vulkan_device_probe()));
    EXPECT_EQ(j["subgroup_size"], "unknown");
    EXPECT_EQ(j["max_shared_memory"], "unknown");
    EXPECT_EQ(j["features"]["cooperative_matrix"], "no");  // extension positively absent
    EXPECT_EQ(j["features"]["shader_float16"], "unknown");
    EXPECT_EQ(j["dtypes"]["i8"]["compute"], "unsupported");
    const json k = caps_to_json(caps_680m());
    EXPECT_EQ(k["subgroup_size"], 64);
    EXPECT_EQ(k["features"]["cooperative_matrix"], "yes");
    EXPECT_EQ(k["features"]["float8_ext"], "no");
    EXPECT_TRUE(k["coopmat_f16_f32_16x16x16_subgroup"].get<bool>());
}

TEST(CapsGate, CoopMatIsGatedByWhatTheDeviceReports) {
    const KernelDesc* k = find_kernel("gemm_coopmat_f16");
    ASSERT_NE(k, nullptr);
    const DeviceCaps c = caps_680m();
    const Shape aligned{64, 32, 48}, ragged{64, 30, 48};
    EXPECT_TRUE(gate_kernel(*k, c, &aligned).eligible);
    EXPECT_FALSE(gate_kernel(*k, c, &ragged).eligible);

    DeviceCaps wave32 = c;
    wave32.subgroup_size = 32;
    EXPECT_FALSE(gate_kernel(*k, wave32).eligible);  // 64-lane workgroup != one subgroup
    DeviceCaps nosub = c;
    nosub.subgroup_size.reset();
    EXPECT_FALSE(gate_kernel(*k, nosub).eligible);
    DeviceCaps noshape = c;
    noshape.coopmat_shapes.clear();
    EXPECT_FALSE(gate_kernel(*k, noshape).eligible);
    DeviceCaps nof16 = c;
    nof16.shader_float16 = Cap::Unknown;
    EXPECT_FALSE(gate_kernel(*k, nof16).eligible);
    EXPECT_FALSE(gate_kernel(*k, caps_from_probe(old_vulkan_device_probe())).eligible);
}

TEST(CapsGate, Int8AndSharedMemoryRequirements) {
    const KernelDesc* i8 = find_kernel("gemm_i8dot_tiled");
    const KernelDesc* f32 = find_kernel("gemm_f32_tiled");
    ASSERT_NE(i8, nullptr);
    ASSERT_NE(f32, nullptr);
    const DeviceCaps c = caps_680m();
    EXPECT_TRUE(gate_kernel(*i8, c).eligible);
    const Shape bad_k{16, 16, 30};
    EXPECT_FALSE(gate_kernel(*i8, c, &bad_k).eligible);  // k % 4 != 0
    DeviceCaps nodot = c;
    nodot.integer_dot_product = Cap::No;
    EXPECT_FALSE(gate_kernel(*i8, nodot).eligible);
    DeviceCaps smallshared = c;
    smallshared.max_shared_memory = 1024;
    EXPECT_FALSE(gate_kernel(*f32, smallshared).eligible);
    DeviceCaps unknownshared = c;
    unknownshared.max_shared_memory.reset();
    const Gate g = gate_kernel(*f32, unknownshared);
    EXPECT_FALSE(g.eligible);
    EXPECT_NE(g.reason.find("unknown"), std::string::npos);
}
