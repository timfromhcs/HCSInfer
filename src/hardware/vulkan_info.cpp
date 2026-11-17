// Vulkan capability report. vulkan-1.dll is loaded dynamically; nothing links against a Vulkan
// import library, so the doctor works (and reports "unavailable") on machines without Vulkan.
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "hw.hpp"

#ifdef _WIN32
#include <windows.h>

#include "win_util.hpp"
#endif

namespace hcs::hardware {

namespace {

std::string hex4(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%04X", v);
    return b;
}
std::string hex8(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}
std::string api_string(std::uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}
std::string cstr(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n] != '\0') ++n;
    return std::string(s, n);
}

const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            return "other";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return "integrated_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return "discrete_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return "virtual_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return "cpu";
        default:
            return "unknown";
    }
}

const char* driver_id_name(std::uint32_t id) {
    switch (id) {
        case 1:
            return "amd_proprietary";
        case 2:
            return "amd_open_source";
        case 3:
            return "mesa_radv";
        case 4:
            return "nvidia_proprietary";
        case 5:
            return "intel_proprietary_windows";
        case 6:
            return "intel_open_source_mesa";
        default:
            return "other_or_unknown";
    }
}

const char* component_type_name(VkComponentTypeKHR t) {
    switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR:
            return "f16";
        case VK_COMPONENT_TYPE_FLOAT32_KHR:
            return "f32";
        case VK_COMPONENT_TYPE_FLOAT64_KHR:
            return "f64";
        case VK_COMPONENT_TYPE_SINT8_KHR:
            return "s8";
        case VK_COMPONENT_TYPE_SINT16_KHR:
            return "s16";
        case VK_COMPONENT_TYPE_SINT32_KHR:
            return "s32";
        case VK_COMPONENT_TYPE_SINT64_KHR:
            return "s64";
        case VK_COMPONENT_TYPE_UINT8_KHR:
            return "u8";
        case VK_COMPONENT_TYPE_UINT16_KHR:
            return "u16";
        case VK_COMPONENT_TYPE_UINT32_KHR:
            return "u32";
        case VK_COMPONENT_TYPE_UINT64_KHR:
            return "u64";
        default:
            return "other";
    }
}

json flag_names(std::uint32_t flags, std::initializer_list<std::pair<std::uint32_t, const char*>> names) {
    json a = json::array();
    for (const auto& [bitv, n] : names)
        if (flags & bitv)
            a.push_back(n);
    return a;
}

json subgroup_ops(std::uint32_t f) {
    return flag_names(f, {{VK_SUBGROUP_FEATURE_BASIC_BIT, "basic"},
                          {VK_SUBGROUP_FEATURE_VOTE_BIT, "vote"},
                          {VK_SUBGROUP_FEATURE_ARITHMETIC_BIT, "arithmetic"},
                          {VK_SUBGROUP_FEATURE_BALLOT_BIT, "ballot"},
                          {VK_SUBGROUP_FEATURE_SHUFFLE_BIT, "shuffle"},
                          {VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT, "shuffle_relative"},
                          {VK_SUBGROUP_FEATURE_CLUSTERED_BIT, "clustered"},
                          {VK_SUBGROUP_FEATURE_QUAD_BIT, "quad"},
                          {VK_SUBGROUP_FEATURE_ROTATE_BIT, "rotate"},
                          {VK_SUBGROUP_FEATURE_ROTATE_CLUSTERED_BIT, "rotate_clustered"}});
}

json stage_names(std::uint32_t f) {
    return flag_names(f, {{VK_SHADER_STAGE_VERTEX_BIT, "vertex"},
                          {VK_SHADER_STAGE_FRAGMENT_BIT, "fragment"},
                          {VK_SHADER_STAGE_COMPUTE_BIT, "compute"},
                          {VK_SHADER_STAGE_GEOMETRY_BIT, "geometry"}});
}

json queue_flag_names(std::uint32_t f) {
    return flag_names(f, {{VK_QUEUE_GRAPHICS_BIT, "graphics"},
                          {VK_QUEUE_COMPUTE_BIT, "compute"},
                          {VK_QUEUE_TRANSFER_BIT, "transfer"},
                          {VK_QUEUE_SPARSE_BINDING_BIT, "sparse_binding"},
                          {VK_QUEUE_PROTECTED_BIT, "protected"}});
}

json mem_prop_names(std::uint32_t f) {
    return flag_names(f, {{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "device_local"},
                          {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "host_visible"},
                          {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "host_coherent"},
                          {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "host_cached"},
                          {VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "lazily_allocated"}});
}

struct Api {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkEnumerateInstanceVersion enum_version = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enum_inst_ext = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices enum_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_props = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 get_props2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 get_feat2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties2 get_mem2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enum_dev_ext = nullptr;
    PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR get_coopmat = nullptr;
};

template <class F>
F load(const Api& api, VkInstance inst, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(api.gipa(inst, name)));
}

json device_report(const Api& api, VkPhysicalDevice pd, std::uint32_t index) {
    json d;
    d["index"] = index;
    VkPhysicalDeviceProperties props{};
    api.get_props(pd, &props);
    const std::uint32_t dev_api = props.apiVersion;
    d["name"] = cstr(props.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    d["device_type"] = device_type_name(props.deviceType);
    d["vendor_id"] = hex4(props.vendorID);
    d["device_id"] = hex4(props.deviceID);
    d["api_version"] = api_string(dev_api);
    d["driver_version_raw"] = hex8(props.driverVersion);

    // Extensions.
    std::set<std::string> exts;
    if (api.enum_dev_ext) {
        std::uint32_t n = 0;
        if (api.enum_dev_ext(pd, nullptr, &n, nullptr) == VK_SUCCESS && n > 0) {
            std::vector<VkExtensionProperties> v(n);
            if (api.enum_dev_ext(pd, nullptr, &n, v.data()) == VK_SUCCESS || n > 0) {
                v.resize(std::min<size_t>(n, v.size()));
                for (const auto& e : v) exts.insert(cstr(e.extensionName, VK_MAX_EXTENSION_NAME_SIZE));
            }
        }
    }
    auto has = [&](const char* e) { return exts.count(e) != 0; };
    d["extension_count"] = exts.size();
    json rel;
    for (const char* e : {"VK_EXT_memory_budget",          "VK_KHR_driver_properties",
                          "VK_KHR_shader_float16_int8",    "VK_KHR_16bit_storage",
                          "VK_KHR_8bit_storage",           "VK_KHR_shader_integer_dot_product",
                          "VK_KHR_cooperative_matrix",     "VK_NV_cooperative_matrix",
                          "VK_EXT_subgroup_size_control",  "VK_KHR_shader_subgroup_extended_types",
                          "VK_KHR_shader_subgroup_rotate", "VK_KHR_maintenance4",
                          "VK_KHR_maintenance5",           "VK_KHR_push_descriptor",
                          "VK_EXT_external_memory_host",   "VK_KHR_timeline_semaphore",
                          "VK_KHR_shader_bfloat16",        "VK_EXT_shader_64bit_indexing",
                          "VK_KHR_buffer_device_address",  "VK_EXT_pipeline_robustness"})
        rel[e] = has(e);
    d["relevant_extensions"] = rel;
    d["all_extensions"] = json(exts);

    const bool v12 = dev_api >= VK_API_VERSION_1_2;
    const bool v13 = dev_api >= VK_API_VERSION_1_3;
    const bool coop = has("VK_KHR_cooperative_matrix");

    if (api.get_props2 && api.get_feat2 && VK_API_VERSION_MINOR(dev_api) >= 1) {
        // --- properties ---
        VkPhysicalDeviceVulkan11Properties p11{};
        p11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
        VkPhysicalDeviceVulkan12Properties p12{};
        p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceVulkan13Properties p13{};
        p13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
        VkPhysicalDeviceProperties2 pr2{};
        pr2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        if (v12) {
            pr2.pNext = &p11;
            p11.pNext = &p12;
            if (v13)
                p12.pNext = &p13;
        }
        api.get_props2(pd, &pr2);
        const VkPhysicalDeviceLimits& L = pr2.properties.limits;

        if (v12) {
            d["driver"] = {{"id", static_cast<std::uint32_t>(p12.driverID)},
                           {"id_name", driver_id_name(static_cast<std::uint32_t>(p12.driverID))},
                           {"name", cstr(p12.driverName, VK_MAX_DRIVER_NAME_SIZE)},
                           {"info", cstr(p12.driverInfo, VK_MAX_DRIVER_INFO_SIZE)},
                           {"conformance_version", std::to_string(p12.conformanceVersion.major) + "." +
                                                       std::to_string(p12.conformanceVersion.minor) + "." +
                                                       std::to_string(p12.conformanceVersion.subminor) + "." +
                                                       std::to_string(p12.conformanceVersion.patch)}};
            d["subgroup"] = {
                {"size", p11.subgroupSize},
                {"supported_stages", stage_names(p11.subgroupSupportedStages)},
                {"supported_operations", subgroup_ops(p11.subgroupSupportedOperations)},
                {"quad_operations_in_all_stages", p11.subgroupQuadOperationsInAllStages == VK_TRUE}};
            if (v13) {
                d["subgroup"]["min_size"] = p13.minSubgroupSize;
                d["subgroup"]["max_size"] = p13.maxSubgroupSize;
                d["subgroup"]["max_compute_workgroup_subgroups"] = p13.maxComputeWorkgroupSubgroups;
                d["subgroup"]["required_size_stages"] = stage_names(p13.requiredSubgroupSizeStages);
                d["max_buffer_size_bytes"] = static_cast<std::uint64_t>(p13.maxBufferSize);
            } else {
                d["subgroup"]["min_size"] = unknown();
                d["subgroup"]["max_size"] = unknown();
            }
            d["max_memory_allocation_size_bytes"] = static_cast<std::uint64_t>(p11.maxMemoryAllocationSize);
        } else {
            d["driver"] = unknown();
            d["subgroup"] = unknown();
            d["max_memory_allocation_size_bytes"] = unknown();
        }
        d["limits"] = {
            {"max_compute_shared_memory_size", L.maxComputeSharedMemorySize},
            {"max_compute_work_group_invocations", L.maxComputeWorkGroupInvocations},
            {"max_compute_work_group_size",
             {L.maxComputeWorkGroupSize[0], L.maxComputeWorkGroupSize[1], L.maxComputeWorkGroupSize[2]}},
            {"max_compute_work_group_count",
             {L.maxComputeWorkGroupCount[0], L.maxComputeWorkGroupCount[1], L.maxComputeWorkGroupCount[2]}},
            {"max_storage_buffer_range", L.maxStorageBufferRange},
            {"max_uniform_buffer_range", L.maxUniformBufferRange},
            {"max_push_constants_size", L.maxPushConstantsSize},
            {"max_bound_descriptor_sets", L.maxBoundDescriptorSets},
            {"max_per_stage_descriptor_storage_buffers", L.maxPerStageDescriptorStorageBuffers},
            {"max_memory_allocation_count", L.maxMemoryAllocationCount},
            {"min_storage_buffer_offset_alignment",
             static_cast<std::uint64_t>(L.minStorageBufferOffsetAlignment)},
            {"non_coherent_atom_size", static_cast<std::uint64_t>(L.nonCoherentAtomSize)},
            {"timestamp_period_ns", L.timestampPeriod},
            {"timestamp_compute_and_graphics", L.timestampComputeAndGraphics == VK_TRUE}};

        // --- features ---
        VkPhysicalDeviceVulkan11Features f11{};
        f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceVulkan12Features f12{};
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR fcm{};
        fcm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
        VkPhysicalDeviceFeatures2 ft2{};
        ft2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        void** tail = &ft2.pNext;
        if (v12) {
            *tail = &f11;
            tail = &f11.pNext;
            *tail = &f12;
            tail = &f12.pNext;
            if (v13) {
                *tail = &f13;
                tail = &f13.pNext;
            }
        }
        if (coop) {
            *tail = &fcm;
            tail = &fcm.pNext;
        }
        api.get_feat2(pd, &ft2);
        json feat;
        feat["shader_int64"] = ft2.features.shaderInt64 == VK_TRUE;
        feat["shader_int16"] = ft2.features.shaderInt16 == VK_TRUE;
        feat["shader_float64"] = ft2.features.shaderFloat64 == VK_TRUE;
        if (v12) {
            feat["storage_buffer_16bit_access"] = f11.storageBuffer16BitAccess == VK_TRUE;
            feat["uniform_and_storage_buffer_16bit_access"] =
                f11.uniformAndStorageBuffer16BitAccess == VK_TRUE;
            feat["shader_float16"] = f12.shaderFloat16 == VK_TRUE;
            feat["shader_int8"] = f12.shaderInt8 == VK_TRUE;
            feat["storage_buffer_8bit_access"] = f12.storageBuffer8BitAccess == VK_TRUE;
            feat["uniform_and_storage_buffer_8bit_access"] = f12.uniformAndStorageBuffer8BitAccess == VK_TRUE;
            feat["timeline_semaphore"] = f12.timelineSemaphore == VK_TRUE;
            feat["buffer_device_address"] = f12.bufferDeviceAddress == VK_TRUE;
            feat["vulkan_memory_model"] = f12.vulkanMemoryModel == VK_TRUE;
            feat["shader_subgroup_extended_types"] = f12.shaderSubgroupExtendedTypes == VK_TRUE;
            feat["scalar_block_layout"] = f12.scalarBlockLayout == VK_TRUE;
        } else {
            for (const char* k :
                 {"storage_buffer_16bit_access", "uniform_and_storage_buffer_16bit_access", "shader_float16",
                  "shader_int8", "storage_buffer_8bit_access", "uniform_and_storage_buffer_8bit_access",
                  "timeline_semaphore", "buffer_device_address", "vulkan_memory_model",
                  "shader_subgroup_extended_types", "scalar_block_layout"})
                feat[k] = "unknown";
        }
        if (v13) {
            feat["shader_integer_dot_product"] = f13.shaderIntegerDotProduct == VK_TRUE;
            feat["subgroup_size_control"] = f13.subgroupSizeControl == VK_TRUE;
            feat["compute_full_subgroups"] = f13.computeFullSubgroups == VK_TRUE;
            feat["maintenance4"] = f13.maintenance4 == VK_TRUE;
            feat["synchronization2"] = f13.synchronization2 == VK_TRUE;
        } else {
            for (const char* k : {"shader_integer_dot_product", "subgroup_size_control",
                                  "compute_full_subgroups", "maintenance4", "synchronization2"})
                feat[k] = "unknown";
        }
        feat["cooperative_matrix_khr"] = coop ? json(fcm.cooperativeMatrix == VK_TRUE) : json(false);
        d["features"] = feat;

        // Cooperative matrix shapes (only when the extension and the feature are present).
        json coop_props = json::array();
        if (coop && fcm.cooperativeMatrix == VK_TRUE && api.get_coopmat) {
            std::uint32_t n = 0;
            if (api.get_coopmat(pd, &n, nullptr) == VK_SUCCESS && n > 0) {
                std::vector<VkCooperativeMatrixPropertiesKHR> v(n);
                for (auto& e : v) e.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
                if (api.get_coopmat(pd, &n, v.data()) >= VK_SUCCESS) {
                    v.resize(std::min<size_t>(n, v.size()));
                    for (const auto& e : v)
                        coop_props.push_back({{"m", e.MSize},
                                              {"n", e.NSize},
                                              {"k", e.KSize},
                                              {"a", component_type_name(e.AType)},
                                              {"b", component_type_name(e.BType)},
                                              {"c", component_type_name(e.CType)},
                                              {"result", component_type_name(e.ResultType)},
                                              {"scope", static_cast<std::uint32_t>(e.scope)}});
                }
            }
        }
        d["cooperative_matrix_properties"] = coop_props;

        // --- memory (with budget if available) ---
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 mp2{};
        mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        const bool have_budget = has("VK_EXT_memory_budget") && api.get_mem2;
        if (have_budget)
            mp2.pNext = &budget;
        if (api.get_mem2)
            api.get_mem2(pd, &mp2);
        json heaps = json::array(), types = json::array();
        for (std::uint32_t i = 0; i < mp2.memoryProperties.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i) {
            const auto& h = mp2.memoryProperties.memoryHeaps[i];
            json e = {{"index", i},
                      {"size_bytes", static_cast<std::uint64_t>(h.size)},
                      {"device_local", (h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0},
                      {"budget_bytes",
                       have_budget ? json(static_cast<std::uint64_t>(budget.heapBudget[i])) : unknown()},
                      {"usage_bytes",
                       have_budget ? json(static_cast<std::uint64_t>(budget.heapUsage[i])) : unknown()}};
            heaps.push_back(std::move(e));
        }
        for (std::uint32_t i = 0; i < mp2.memoryProperties.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; ++i) {
            const auto& t = mp2.memoryProperties.memoryTypes[i];
            types.push_back(
                {{"index", i}, {"heap_index", t.heapIndex}, {"properties", mem_prop_names(t.propertyFlags)}});
        }
        d["memory"] = {{"memory_budget_extension", have_budget}, {"heaps", heaps}, {"types", types}};
    } else {
        d["limits"] = unknown();
        d["features"] = unknown();
        d["memory"] = unknown();
        d["note"] = "Vulkan 1.1+ property/feature queries unavailable on this device/loader";
    }

    // --- queue families ---
    json queues = json::array();
    if (api.get_queue) {
        std::uint32_t n = 0;
        api.get_queue(pd, &n, nullptr);
        std::vector<VkQueueFamilyProperties> q(n);
        if (n > 0)
            api.get_queue(pd, &n, q.data());
        for (std::uint32_t i = 0; i < n && i < q.size(); ++i)
            queues.push_back({{"index", i},
                              {"queue_count", q[i].queueCount},
                              {"flags", queue_flag_names(q[i].queueFlags)},
                              {"timestamp_valid_bits", q[i].timestampValidBits}});
    }
    d["queue_families"] = queues;
    return d;
}

}  // namespace

json vulkan_report(bool skip) {
    json r;
    if (skip) {
        r["status"] = "skipped";
        r["reason"] = "--no-vulkan (CPU-only mode; vulkan-1.dll was not loaded)";
        r["devices"] = json::array();
        return r;
    }
    r["devices"] = json::array();
#ifndef _WIN32
    r["status"] = "unavailable";
    r["reason"] = "dynamic Vulkan loading is only implemented for Windows in this build";
    return r;
#else
    HMODULE lib = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) {
        r["status"] = "unavailable";
        r["reason"] = "vulkan-1.dll (Vulkan loader) not found; install/update the GPU driver";
        return r;
    }
    {
        wchar_t path[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(lib, path, MAX_PATH);
        r["loader_path"] = n > 0 && n < MAX_PATH ? json(win::narrow(std::wstring(path, n))) : unknown();
    }
    Api api;
    api.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
    if (!api.gipa) {
        r["status"] = "unavailable";
        r["reason"] = "vkGetInstanceProcAddr not exported by vulkan-1.dll";
        return r;
    }
    api.create_instance = load<PFN_vkCreateInstance>(api, nullptr, "vkCreateInstance");
    api.enum_version = load<PFN_vkEnumerateInstanceVersion>(api, nullptr, "vkEnumerateInstanceVersion");
    api.enum_inst_ext = load<PFN_vkEnumerateInstanceExtensionProperties>(
        api, nullptr, "vkEnumerateInstanceExtensionProperties");
    if (!api.create_instance) {
        r["status"] = "unavailable";
        r["reason"] = "vkCreateInstance could not be resolved";
        return r;
    }
    std::uint32_t inst_ver = VK_API_VERSION_1_0;
    if (api.enum_version)
        api.enum_version(&inst_ver);
    r["instance_version"] = api_string(inst_ver);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "HCSInfer doctor";
    app.apiVersion = inst_ver;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst = nullptr;
    const VkResult cr = api.create_instance(&ci, nullptr, &inst);
    if (cr != VK_SUCCESS || !inst) {
        r["status"] = "failed";
        r["reason"] = "vkCreateInstance failed with VkResult " + std::to_string(static_cast<int>(cr));
        return r;
    }
    api.destroy_instance = load<PFN_vkDestroyInstance>(api, inst, "vkDestroyInstance");
    api.enum_devices = load<PFN_vkEnumeratePhysicalDevices>(api, inst, "vkEnumeratePhysicalDevices");
    api.get_props = load<PFN_vkGetPhysicalDeviceProperties>(api, inst, "vkGetPhysicalDeviceProperties");
    api.get_props2 = load<PFN_vkGetPhysicalDeviceProperties2>(api, inst, "vkGetPhysicalDeviceProperties2");
    api.get_feat2 = load<PFN_vkGetPhysicalDeviceFeatures2>(api, inst, "vkGetPhysicalDeviceFeatures2");
    api.get_mem2 =
        load<PFN_vkGetPhysicalDeviceMemoryProperties2>(api, inst, "vkGetPhysicalDeviceMemoryProperties2");
    api.get_queue = load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        api, inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    api.enum_dev_ext =
        load<PFN_vkEnumerateDeviceExtensionProperties>(api, inst, "vkEnumerateDeviceExtensionProperties");
    api.get_coopmat = load<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
        api, inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    if (!api.enum_devices || !api.get_props) {
        r["status"] = "failed";
        r["reason"] = "required instance-level entry points could not be resolved";
        if (api.destroy_instance)
            api.destroy_instance(inst, nullptr);
        return r;
    }
    std::uint32_t n = 0;
    VkResult er = api.enum_devices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (er == VK_SUCCESS && n > 0)
        er = api.enum_devices(inst, &n, devs.data());
    if (er != VK_SUCCESS && er != VK_INCOMPLETE) {
        r["status"] = "failed";
        r["reason"] =
            "vkEnumeratePhysicalDevices failed with VkResult " + std::to_string(static_cast<int>(er));
        api.destroy_instance(inst, nullptr);
        return r;
    }
    devs.resize(std::min<size_t>(n, devs.size()));
    for (std::uint32_t i = 0; i < devs.size(); ++i) r["devices"].push_back(device_report(api, devs[i], i));
    r["device_count"] = devs.size();
    r["status"] = devs.empty() ? "no_devices" : "ok";
    if (devs.empty())
        r["reason"] = "Vulkan loader works but reports no physical devices";
    if (api.destroy_instance)
        api.destroy_instance(inst, nullptr);
    // vulkan-1.dll is intentionally not unloaded (driver threads may still be shutting down).
    return r;
#endif
}

}  // namespace hcs::hardware
