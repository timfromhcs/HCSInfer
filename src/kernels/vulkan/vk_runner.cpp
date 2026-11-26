// Standalone Vulkan compute runner (see include/hcs/kernels/vk_runner.hpp).
#include "hcs/kernels/vk_runner.hpp"

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hcs::kernels::vulkan {

namespace {

// ---- dynamic loader -------------------------------------------------------------------------------------
#define HCS_VK_INSTANCE_FUNCS(X)          \
    X(DestroyInstance)                    \
    X(EnumeratePhysicalDevices)           \
    X(GetPhysicalDeviceProperties)        \
    X(GetPhysicalDeviceMemoryProperties)  \
    X(GetPhysicalDeviceFeatures2)         \
    X(EnumerateDeviceExtensionProperties) \
    X(CreateDevice)                       \
    X(GetDeviceProcAddr)

#define HCS_VK_DEVICE_FUNCS(X)     \
    X(DestroyDevice)               \
    X(GetDeviceQueue)              \
    X(DeviceWaitIdle)              \
    X(CreateBuffer)                \
    X(DestroyBuffer)               \
    X(GetBufferMemoryRequirements) \
    X(AllocateMemory)              \
    X(FreeMemory)                  \
    X(BindBufferMemory)            \
    X(MapMemory)                   \
    X(UnmapMemory)                 \
    X(CreateShaderModule)          \
    X(DestroyShaderModule)         \
    X(CreateDescriptorSetLayout)   \
    X(DestroyDescriptorSetLayout)  \
    X(CreateDescriptorPool)        \
    X(DestroyDescriptorPool)       \
    X(ResetDescriptorPool)         \
    X(AllocateDescriptorSets)      \
    X(UpdateDescriptorSets)        \
    X(CreatePipelineLayout)        \
    X(DestroyPipelineLayout)       \
    X(CreateComputePipelines)      \
    X(DestroyPipeline)             \
    X(CreateCommandPool)           \
    X(DestroyCommandPool)          \
    X(AllocateCommandBuffers)      \
    X(ResetCommandBuffer)          \
    X(BeginCommandBuffer)          \
    X(EndCommandBuffer)            \
    X(CmdBindPipeline)             \
    X(CmdBindDescriptorSets)       \
    X(CmdPushConstants)            \
    X(CmdDispatch)                 \
    X(CmdPipelineBarrier)          \
    X(CmdCopyBuffer)               \
    X(CreateFence)                 \
    X(DestroyFence)                \
    X(ResetFences)                 \
    X(WaitForFences)               \
    X(QueueSubmit)                 \
    X(CreateQueryPool)             \
    X(DestroyQueryPool)            \
    X(CmdResetQueryPool)           \
    X(CmdWriteTimestamp)           \
    X(GetQueryPoolResults)

struct Api {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
    PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
#define X(n) PFN_vk##n n = nullptr;
    HCS_VK_INSTANCE_FUNCS(X)
    HCS_VK_DEVICE_FUNCS(X)
#undef X
    PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT = nullptr;
};

#ifdef _WIN32
// The loader library is process-lifetime: driver threads may still be shutting down, so it is never unloaded.
PFN_vkGetInstanceProcAddr load_gipa() {
    static PFN_vkGetInstanceProcAddr gipa = [] {
        HMODULE lib = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib)
            return static_cast<PFN_vkGetInstanceProcAddr>(nullptr);
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
    }();
    return gipa;
}
#else
PFN_vkGetInstanceProcAddr load_gipa() {
    return nullptr;
}
#endif

template <class H>
std::uint64_t to_u64(H h) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h));
}
template <class H>
H from_u64(std::uint64_t v) {
    return reinterpret_cast<H>(static_cast<std::uintptr_t>(v));
}

Error be(const std::string& what, VkResult r) {
    return make_error(ErrorCode::BackendError,
                      what + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                        VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

}  // namespace

namespace detail {

struct Impl {
    RunnerOptions opt;
    hardware::DeviceCaps caps;
    Api api;

    VkInstance instance = nullptr;
    VkDebugUtilsMessengerEXT messenger = nullptr;
    VkPhysicalDevice phys = nullptr;
    VkDevice device = nullptr;
    VkQueue queue = nullptr;
    std::uint32_t qfam = 0;
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cmd = nullptr;
    VkFence fence = nullptr;
    VkQueryPool qpool = nullptr;
    VkDescriptorPool dpool = nullptr;
    std::uint32_t timestamp_valid_bits = 0;
    float timestamp_period_ns = 0.0f;
    std::uint32_t max_push = 0;
    int type_device_local = -1;  // DEVICE_LOCAL, not host visible
    int type_host = -1;          // HOST_VISIBLE | HOST_COHERENT
    VkPhysicalDeviceMemoryProperties mem{};

    std::mutex mu;
    std::uint64_t generation = 1;
    bool ready = false;
    bool lost = false;
    Fault fault = Fault::None;

    std::map<std::uint64_t, std::uint64_t> live_buffers;  // VkBuffer -> VkDeviceMemory
    struct PipeRes {
        std::uint64_t module, dsl, layout;
    };
    std::map<std::uint64_t, PipeRes> live_pipelines;  // VkPipeline -> aux objects

    // validation
    bool val_requested = false, val_active = false;
    std::string val_note;
    std::atomic<std::uint64_t> val_messages{0};
    std::mutex val_mu;
    std::vector<std::string> val_first;

    ~Impl() { teardown(); }

    Result<void> init();
    void teardown() noexcept;
    Result<std::uint64_t> make_raw_buffer(std::size_t bytes, bool host, VkBufferUsageFlags usage,
                                          std::uint64_t& memory_out, void** mapped_out);
    void free_raw_buffer(std::uint64_t buffer, std::uint64_t memory, bool mapped) noexcept;
    Result<void> submit_and_wait(VkCommandBuffer c, double* wall_ms);
    void mark_lost() { lost = true; }
};

}  // namespace detail

namespace {

using detail::Impl;

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                        VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0)
        return VK_FALSE;
    auto* impl = static_cast<Impl*>(user);
    impl->val_messages.fetch_add(1);
    std::lock_guard<std::mutex> lk(impl->val_mu);
    if (impl->val_first.size() < 8)
        impl->val_first.push_back(data && data->pMessage ? std::string(data->pMessage).substr(0, 600)
                                                         : std::string("?"));
    return VK_FALSE;
}

bool has_layer(const Api& a, const char* name) {
    if (!a.EnumerateInstanceLayerProperties)
        return false;
    std::uint32_t n = 0;
    if (a.EnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS || n == 0)
        return false;
    std::vector<VkLayerProperties> v(n);
    if (a.EnumerateInstanceLayerProperties(&n, v.data()) < VK_SUCCESS)
        return false;
    v.resize(std::min<std::size_t>(n, v.size()));
    return std::any_of(v.begin(), v.end(),
                       [&](const VkLayerProperties& p) { return std::strcmp(p.layerName, name) == 0; });
}

bool has_instance_ext(const Api& a, const char* name) {
    if (!a.EnumerateInstanceExtensionProperties)
        return false;
    std::uint32_t n = 0;
    if (a.EnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS || n == 0)
        return false;
    std::vector<VkExtensionProperties> v(n);
    if (a.EnumerateInstanceExtensionProperties(nullptr, &n, v.data()) < VK_SUCCESS)
        return false;
    v.resize(std::min<std::size_t>(n, v.size()));
    return std::any_of(v.begin(), v.end(), [&](const VkExtensionProperties& p) {
        return std::strcmp(p.extensionName, name) == 0;
    });
}

}  // namespace

namespace detail {

Result<void> Impl::init() {
    if (ready)
        return {};
    lost = false;
    fault = Fault::None;

    // Capability record from the doctor's probe (own instance, torn down before ours is created).
    std::string status, reason;
    auto devices = hardware::probe_vulkan_caps(&status, &reason);
    if (devices.empty())
        return make_error(ErrorCode::Unsupported, "no Vulkan device available (" + status +
                                                      (reason.empty() ? "" : ": " + reason) + ")");
    if (opt.device_index >= devices.size())
        return make_error(ErrorCode::InvalidArgument,
                          "Vulkan device index " + std::to_string(opt.device_index) + " out of range (have " +
                              std::to_string(devices.size()) + ")");
    caps = devices[opt.device_index];
    if (!caps.compute_queue_family())
        return make_error(ErrorCode::Unsupported, "device " + caps.name + " reports no compute queue family");
    qfam = *caps.compute_queue_family();

    const PFN_vkGetInstanceProcAddr gipa = load_gipa();
    if (!gipa)
        return make_error(ErrorCode::Unsupported, "Vulkan loader (vulkan-1.dll) not available");
    api.gipa = gipa;
    api.CreateInstance =
        reinterpret_cast<PFN_vkCreateInstance>(reinterpret_cast<void*>(gipa(nullptr, "vkCreateInstance")));
    api.EnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        reinterpret_cast<void*>(gipa(nullptr, "vkEnumerateInstanceVersion")));
    api.EnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        reinterpret_cast<void*>(gipa(nullptr, "vkEnumerateInstanceLayerProperties")));
    api.EnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        reinterpret_cast<void*>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties")));
    if (!api.CreateInstance)
        return make_error(ErrorCode::Unsupported, "vkCreateInstance not resolvable");

    std::uint32_t inst_ver = VK_API_VERSION_1_0;
    if (api.EnumerateInstanceVersion)
        api.EnumerateInstanceVersion(&inst_ver);
    if (inst_ver < VK_API_VERSION_1_1)
        return make_error(ErrorCode::Unsupported,
                          "Vulkan instance < 1.1 is not supported by the HCS kernels");
    const std::uint32_t app_ver = std::min<std::uint32_t>(inst_ver, VK_API_VERSION_1_3);

    // Validation layer.
    val_requested = opt.validation;
    val_active = false;
    val_note.clear();
    val_messages = 0;
    {
        std::lock_guard<std::mutex> lk(val_mu);
        val_first.clear();
    }
    std::vector<const char*> layers, inst_exts;
    bool use_messenger = false;
    if (opt.validation) {
        static const char* kLayer = "VK_LAYER_KHRONOS_validation";
#ifdef _WIN32
        if (!has_layer(api, kLayer)) {
            // The SDK ships the layer next to its tools; point the loader there (this process only).
            char sdk[MAX_PATH]{};
            const DWORD n = GetEnvironmentVariableA("VULKAN_SDK", sdk, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                const std::string bin = std::string(sdk) + "\\Bin";
                SetEnvironmentVariableA("VK_ADD_LAYER_PATH", bin.c_str());
            }
        }
#endif
        if (has_layer(api, kLayer) && has_instance_ext(api, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            layers.push_back(kLayer);
            inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            use_messenger = true;
            val_active = true;
        } else {
            val_note = "VK_LAYER_KHRONOS_validation / VK_EXT_debug_utils not available: validation NOT RUN";
        }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "HCSInfer kernel runner";
    app.apiVersion = app_ver;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    ici.enabledExtensionCount = static_cast<std::uint32_t>(inst_exts.size());
    ici.ppEnabledExtensionNames = inst_exts.empty() ? nullptr : inst_exts.data();
    if (VkResult r = api.CreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS || !instance) {
        instance = nullptr;
        return be("vkCreateInstance", r);
    }
    auto li = [&](const char* name) { return reinterpret_cast<void*>(gipa(instance, name)); };
#define X(n) api.n = reinterpret_cast<PFN_vk##n>(li("vk" #n));
    HCS_VK_INSTANCE_FUNCS(X)
#undef X
    if (!api.DestroyInstance || !api.EnumeratePhysicalDevices || !api.GetDeviceProcAddr ||
        !api.CreateDevice) {
        teardown();
        return make_error(ErrorCode::BackendError, "required Vulkan instance entry points are missing");
    }
    if (use_messenger) {
        api.CreateDebugUtilsMessengerEXT =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(li("vkCreateDebugUtilsMessengerEXT"));
        api.DestroyDebugUtilsMessengerEXT =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(li("vkDestroyDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mi{};
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = debug_cb;
        mi.pUserData = this;
        if (!api.CreateDebugUtilsMessengerEXT ||
            api.CreateDebugUtilsMessengerEXT(instance, &mi, nullptr, &messenger) != VK_SUCCESS) {
            messenger = nullptr;
            val_active = false;
            val_note =
                "validation layer loaded but the debug messenger could not be created: validation NOT RUN";
        }
    }

    std::uint32_t n = 0;
    api.EnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    if (n > 0)
        api.EnumeratePhysicalDevices(instance, &n, pds.data());
    pds.resize(std::min<std::size_t>(n, pds.size()));
    if (opt.device_index >= pds.size()) {
        teardown();
        return make_error(ErrorCode::BackendError,
                          "device enumeration of the runner instance differs from the probe");
    }
    phys = pds[opt.device_index];
    VkPhysicalDeviceProperties props{};
    api.GetPhysicalDeviceProperties(phys, &props);
    if (props.vendorID != caps.vendor_id || props.deviceID != caps.device_id) {
        teardown();
        return make_error(ErrorCode::BackendError, "physical device order changed between probe and runner");
    }
    timestamp_period_ns = props.limits.timestampPeriod;
    max_push = props.limits.maxPushConstantsSize;
    api.GetPhysicalDeviceMemoryProperties(phys, &mem);
    const std::uint32_t dev_ver = props.apiVersion;

    // Extensions the device really has.
    std::vector<std::string> dev_exts;
    {
        std::uint32_t en = 0;
        api.EnumerateDeviceExtensionProperties(phys, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ev(en);
        if (en > 0 && api.EnumerateDeviceExtensionProperties(phys, nullptr, &en, ev.data()) >= VK_SUCCESS) {
            ev.resize(std::min<std::size_t>(en, ev.size()));
            for (const auto& e : ev) dev_exts.emplace_back(e.extensionName);
        }
    }
    auto dev_has = [&](const char* name) {
        return std::find(dev_exts.begin(), dev_exts.end(), name) != dev_exts.end();
    };

    // Query what the device supports, then enable only what the HCS kernels need AND the device reports.
    const bool v12 = dev_ver >= VK_API_VERSION_1_2, v13 = dev_ver >= VK_API_VERSION_1_3;
    const bool coop_ext = dev_has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    const bool dot_ext = v13 || dev_has(VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);

    VkPhysicalDeviceVulkan11Features q11{};
    q11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features q12{};
    q12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceShaderIntegerDotProductFeatures qdot{};
    qdot.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR qcoop{};
    qcoop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 q2{};
    q2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    void** tail = &q2.pNext;
    if (v12) {
        *tail = &q11;
        tail = &q11.pNext;
        *tail = &q12;
        tail = &q12.pNext;
    }
    if (dot_ext) {
        *tail = &qdot;
        tail = &qdot.pNext;
    }
    if (coop_ext) {
        *tail = &qcoop;
        tail = &qcoop.pNext;
    }
    if (api.GetPhysicalDeviceFeatures2)
        api.GetPhysicalDeviceFeatures2(phys, &q2);

    VkPhysicalDeviceVulkan11Features e11{};
    e11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    e11.storageBuffer16BitAccess = q11.storageBuffer16BitAccess;
    VkPhysicalDeviceVulkan12Features e12{};
    e12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    e12.shaderFloat16 = q12.shaderFloat16;
    e12.vulkanMemoryModel = q12.vulkanMemoryModel;
    VkPhysicalDeviceShaderIntegerDotProductFeatures edot{};
    edot.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES;
    edot.shaderIntegerDotProduct = qdot.shaderIntegerDotProduct;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR ecoop{};
    ecoop.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    ecoop.cooperativeMatrix = qcoop.cooperativeMatrix;
    VkPhysicalDeviceFeatures2 e2{};
    e2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    tail = &e2.pNext;
    if (v12) {
        *tail = &e11;
        tail = &e11.pNext;
        *tail = &e12;
        tail = &e12.pNext;
    }
    std::vector<const char*> enable_exts;
    if (dot_ext && edot.shaderIntegerDotProduct) {
        *tail = &edot;
        tail = &edot.pNext;
        if (!v13)
            enable_exts.push_back(VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME);
    }
    if (coop_ext && ecoop.cooperativeMatrix) {
        *tail = &ecoop;
        tail = &ecoop.pNext;
        enable_exts.push_back(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &e2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(enable_exts.size());
    dci.ppEnabledExtensionNames = enable_exts.empty() ? nullptr : enable_exts.data();
    if (VkResult r = api.CreateDevice(phys, &dci, nullptr, &device); r != VK_SUCCESS || !device) {
        device = nullptr;
        teardown();
        return be("vkCreateDevice", r);
    }
    auto ld = [&](const char* name) { return reinterpret_cast<void*>(api.GetDeviceProcAddr(device, name)); };
#define X(n) api.n = reinterpret_cast<PFN_vk##n>(ld("vk" #n));
    HCS_VK_DEVICE_FUNCS(X)
#undef X
    bool missing = false;
#define X(n)    \
    if (!api.n) \
        missing = true;
    HCS_VK_DEVICE_FUNCS(X)
#undef X
    if (missing) {
        teardown();
        return make_error(ErrorCode::BackendError, "required Vulkan device entry points are missing");
    }
    api.GetDeviceQueue(device, qfam, 0, &queue);

    // Memory types.
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
        if (type_device_local < 0 && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            type_device_local = static_cast<int>(i);
        const VkMemoryPropertyFlags want =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (type_host < 0 && (f & want) == want)
            type_host = static_cast<int>(i);
    }
    if (type_host < 0) {
        teardown();
        return make_error(ErrorCode::BackendError, "no HOST_VISIBLE|HOST_COHERENT memory type");
    }

    // Command pool/buffer, fence, query pool, descriptor pool.
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = qfam;
    if (VkResult r = api.CreateCommandPool(device, &cpi, nullptr, &cmd_pool); r != VK_SUCCESS) {
        cmd_pool = nullptr;
        teardown();
        return be("vkCreateCommandPool", r);
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (VkResult r = api.AllocateCommandBuffers(device, &cai, &cmd); r != VK_SUCCESS) {
        cmd = nullptr;
        teardown();
        return be("vkAllocateCommandBuffers", r);
    }
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (VkResult r = api.CreateFence(device, &fi, nullptr, &fence); r != VK_SUCCESS) {
        fence = nullptr;
        teardown();
        return be("vkCreateFence", r);
    }
    // Timestamps: the first compute family reports valid bits (decoded in caps.timestamps).
    {
        // valid bits of the compute family come from the probe's queue record
        timestamp_valid_bits = 0;
        for (const auto& q : caps.queues)
            if (q.index == qfam)
                timestamp_valid_bits = q.timestamp_valid_bits;
        if (timestamp_valid_bits > 0 && timestamp_period_ns > 0.0f) {
            VkQueryPoolCreateInfo qi{};
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            if (api.CreateQueryPool(device, &qi, nullptr, &qpool) != VK_SUCCESS)
                qpool = nullptr;
        }
    }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (VkResult r = api.CreateDescriptorPool(device, &dpi, nullptr, &dpool); r != VK_SUCCESS) {
        dpool = nullptr;
        teardown();
        return be("vkCreateDescriptorPool", r);
    }
    ready = true;
    return {};
}

void Impl::teardown() noexcept {
    if (device && api.DeviceWaitIdle)
        api.DeviceWaitIdle(device);
    if (device) {
        for (auto& [pipe, res] : live_pipelines) {
            api.DestroyPipeline(device, from_u64<VkPipeline>(pipe), nullptr);
            api.DestroyPipelineLayout(device, from_u64<VkPipelineLayout>(res.layout), nullptr);
            api.DestroyDescriptorSetLayout(device, from_u64<VkDescriptorSetLayout>(res.dsl), nullptr);
            api.DestroyShaderModule(device, from_u64<VkShaderModule>(res.module), nullptr);
        }
        live_pipelines.clear();
        for (auto& [buf, memh] : live_buffers) {
            api.DestroyBuffer(device, from_u64<VkBuffer>(buf), nullptr);
            api.FreeMemory(device, from_u64<VkDeviceMemory>(memh), nullptr);
        }
        live_buffers.clear();
        if (dpool)
            api.DestroyDescriptorPool(device, dpool, nullptr);
        if (qpool)
            api.DestroyQueryPool(device, qpool, nullptr);
        if (fence)
            api.DestroyFence(device, fence, nullptr);
        if (cmd_pool)
            api.DestroyCommandPool(device, cmd_pool, nullptr);  // frees the command buffer
        if (api.DestroyDevice)
            api.DestroyDevice(device, nullptr);
    }
    dpool = nullptr;
    qpool = nullptr;
    fence = nullptr;
    cmd = nullptr;
    cmd_pool = nullptr;
    device = nullptr;
    queue = nullptr;
    if (instance) {
        if (messenger && api.DestroyDebugUtilsMessengerEXT)
            api.DestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        messenger = nullptr;
        if (api.DestroyInstance)
            api.DestroyInstance(instance, nullptr);
    }
    instance = nullptr;
    phys = nullptr;
    type_device_local = type_host = -1;
    ready = false;
    ++generation;  // every Buffer/Pipeline created before now is stale
}

Result<std::uint64_t> Impl::make_raw_buffer(std::size_t bytes, bool host, VkBufferUsageFlags usage,
                                            std::uint64_t& memory_out, void** mapped_out) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer b = nullptr;
    if (VkResult r = api.CreateBuffer(device, &bi, nullptr, &b); r != VK_SUCCESS)
        return be("vkCreateBuffer", r);
    VkMemoryRequirements req{};
    api.GetBufferMemoryRequirements(device, b, &req);
    int type = host ? type_host : type_device_local;
    if (type < 0 || (req.memoryTypeBits & (1u << static_cast<unsigned>(type))) == 0) {
        api.DestroyBuffer(device, b, nullptr);
        return make_error(ErrorCode::BackendError, "no suitable memory type for the buffer");
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = static_cast<std::uint32_t>(type);
    VkDeviceMemory m = nullptr;
    if (VkResult r = api.AllocateMemory(device, &ai, nullptr, &m); r != VK_SUCCESS) {
        api.DestroyBuffer(device, b, nullptr);
        return make_error(r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY
                              ? ErrorCode::OutOfMemory
                              : ErrorCode::BackendError,
                          "vkAllocateMemory failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
    if (VkResult r = api.BindBufferMemory(device, b, m, 0); r != VK_SUCCESS) {
        api.FreeMemory(device, m, nullptr);
        api.DestroyBuffer(device, b, nullptr);
        return be("vkBindBufferMemory", r);
    }
    void* mapped = nullptr;
    if (host) {
        if (VkResult r = api.MapMemory(device, m, 0, VK_WHOLE_SIZE, 0, &mapped); r != VK_SUCCESS) {
            api.FreeMemory(device, m, nullptr);
            api.DestroyBuffer(device, b, nullptr);
            return be("vkMapMemory", r);
        }
    }
    memory_out = to_u64(m);
    if (mapped_out)
        *mapped_out = mapped;
    return to_u64(b);
}

void Impl::free_raw_buffer(std::uint64_t buffer, std::uint64_t memory, bool mapped) noexcept {
    if (mapped)
        api.UnmapMemory(device, from_u64<VkDeviceMemory>(memory));
    api.DestroyBuffer(device, from_u64<VkBuffer>(buffer), nullptr);
    api.FreeMemory(device, from_u64<VkDeviceMemory>(memory), nullptr);
}

Result<void> Impl::submit_and_wait(VkCommandBuffer c, double* wall_ms) {
    if (fault == Fault::DeviceLostOnSubmit) {
        fault = Fault::None;
        mark_lost();
        return make_error(ErrorCode::BackendError, "vkQueueSubmit failed (injected VK_ERROR_DEVICE_LOST)");
    }
    api.ResetFences(device, 1, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    const auto t0 = std::chrono::steady_clock::now();
    if (VkResult r = api.QueueSubmit(queue, 1, &si, fence); r != VK_SUCCESS) {
        mark_lost();
        return be("vkQueueSubmit", r);
    }
    const auto timeout_ns = static_cast<std::uint64_t>(std::max(opt.dispatch_timeout_s, 0.001) * 1e9);
    const VkResult w = api.WaitForFences(device, 1, &fence, VK_TRUE, timeout_ns);
    const auto t1 = std::chrono::steady_clock::now();
    if (wall_ms)
        *wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (w == VK_TIMEOUT) {
        mark_lost();
        return make_error(ErrorCode::Timeout, "dispatch did not finish within " +
                                                  std::to_string(opt.dispatch_timeout_s) +
                                                  " s; runner marked lost (call reset())");
    }
    if (w != VK_SUCCESS) {
        mark_lost();
        return be("vkWaitForFences", w);
    }
    return {};
}

}  // namespace detail

namespace {
Error lost_error() {
    return make_error(ErrorCode::InvalidState,
                      "the Vulkan runner is lost (device lost / timeout); call reset()");
}
Error stale_error() {
    return make_error(ErrorCode::InvalidState,
                      "the Buffer/Pipeline belongs to a torn-down device (runner was reset)");
}
}  // namespace

// ---- Buffer / Pipeline RAII --------------------------------------------------------------------------
void Buffer::release() noexcept {
    if (impl_) {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (generation_ == impl_->generation && impl_->device && buffer_ != 0) {
            impl_->live_buffers.erase(buffer_);
            impl_->free_raw_buffer(buffer_, memory_, mapped_ != nullptr);
        }
    }
    impl_.reset();
    buffer_ = memory_ = 0;
    mapped_ = nullptr;
    size_ = 0;
}
Buffer::~Buffer() {
    release();
}
Buffer::Buffer(Buffer&& o) noexcept
    : impl_(std::move(o.impl_)),
      buffer_(o.buffer_),
      memory_(o.memory_),
      mapped_(o.mapped_),
      size_(o.size_),
      generation_(o.generation_) {
    o.buffer_ = o.memory_ = 0;
    o.mapped_ = nullptr;
    o.size_ = 0;
}
Buffer& Buffer::operator=(Buffer&& o) noexcept {
    if (this != &o) {
        release();
        impl_ = std::move(o.impl_);
        buffer_ = o.buffer_;
        memory_ = o.memory_;
        mapped_ = o.mapped_;
        size_ = o.size_;
        generation_ = o.generation_;
        o.buffer_ = o.memory_ = 0;
        o.mapped_ = nullptr;
        o.size_ = 0;
    }
    return *this;
}

void Pipeline::release() noexcept {
    if (impl_) {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (generation_ == impl_->generation && impl_->device && pipeline_ != 0) {
            impl_->live_pipelines.erase(pipeline_);
            auto& a = impl_->api;
            a.DestroyPipeline(impl_->device, from_u64<VkPipeline>(pipeline_), nullptr);
            a.DestroyPipelineLayout(impl_->device, from_u64<VkPipelineLayout>(layout_), nullptr);
            a.DestroyDescriptorSetLayout(impl_->device, from_u64<VkDescriptorSetLayout>(dsl_), nullptr);
            a.DestroyShaderModule(impl_->device, from_u64<VkShaderModule>(module_), nullptr);
        }
    }
    impl_.reset();
    module_ = dsl_ = layout_ = pipeline_ = 0;
}
Pipeline::~Pipeline() {
    release();
}
Pipeline::Pipeline(Pipeline&& o) noexcept
    : impl_(std::move(o.impl_)),
      module_(o.module_),
      dsl_(o.dsl_),
      layout_(o.layout_),
      pipeline_(o.pipeline_),
      num_buffers_(o.num_buffers_),
      push_bytes_(o.push_bytes_),
      generation_(o.generation_) {
    o.module_ = o.dsl_ = o.layout_ = o.pipeline_ = 0;
}
Pipeline& Pipeline::operator=(Pipeline&& o) noexcept {
    if (this != &o) {
        release();
        impl_ = std::move(o.impl_);
        module_ = o.module_;
        dsl_ = o.dsl_;
        layout_ = o.layout_;
        pipeline_ = o.pipeline_;
        num_buffers_ = o.num_buffers_;
        push_bytes_ = o.push_bytes_;
        generation_ = o.generation_;
        o.module_ = o.dsl_ = o.layout_ = o.pipeline_ = 0;
    }
    return *this;
}

// ---- Runner -----------------------------------------------------------------------------------------
Result<Runner> Runner::create(const RunnerOptions& options) {
    Runner r;
    r.options_ = options;
    r.impl_ = std::make_shared<detail::Impl>();
    r.impl_->opt = options;
    if (auto st = r.impl_->init(); !st.ok())
        return st.error();
    return r;
}

Runner::Runner(Runner&&) noexcept = default;
Runner& Runner::operator=(Runner&& o) noexcept {
    if (this != &o) {
        if (impl_) {
            std::lock_guard<std::mutex> lk(impl_->mu);
            impl_->teardown();
        }
        impl_ = std::move(o.impl_);
        options_ = o.options_;
    }
    return *this;
}
Runner::~Runner() {
    if (impl_) {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->teardown();
    }
}

const hardware::DeviceCaps& Runner::caps() const {
    return impl_->caps;
}
bool Runner::device_local_buffers() const {
    return impl_->opt.prefer_device_local && impl_->type_device_local >= 0;
}
bool Runner::timestamps_available() const {
    return impl_->qpool != nullptr;
}
bool Runner::lost() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->lost || !impl_->ready;
}
ValidationReport Runner::validation() const {
    ValidationReport v;
    v.requested = impl_->val_requested;
    v.active = impl_->val_active;
    v.messages = impl_->val_messages.load();
    v.note = impl_->val_note;
    std::lock_guard<std::mutex> lk(impl_->val_mu);
    v.first_messages = impl_->val_first;
    return v;
}
void Runner::inject_fault_for_testing(Fault f) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->fault = f;
}

Result<Buffer> Runner::create_buffer(std::size_t bytes) {
    if (bytes == 0)
        return make_error(ErrorCode::InvalidArgument, "zero-sized buffer");
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->lost || !impl_->ready)
        return lost_error();
    const bool host = !(impl_->opt.prefer_device_local && impl_->type_device_local >= 0);
    std::uint64_t mem = 0;
    void* mapped = nullptr;
    auto raw = impl_->make_raw_buffer(bytes, host,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      mem, &mapped);
    if (!raw)
        return raw.error();
    Buffer b;
    b.impl_ = impl_;
    b.buffer_ = raw.value();
    b.memory_ = mem;
    b.mapped_ = mapped;
    b.size_ = bytes;
    b.generation_ = impl_->generation;
    impl_->live_buffers[b.buffer_] = mem;
    return b;
}

namespace {
void global_barrier(Impl& s, VkCommandBuffer c, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                    VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    s.api.CmdPipelineBarrier(c, src_stage, dst_stage, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
}  // namespace

Result<void> Runner::upload(Buffer& dst, const void* src, std::size_t bytes) {
    if (bytes > dst.size_ || (bytes > 0 && src == nullptr))
        return make_error(ErrorCode::InvalidArgument, "upload range exceeds the buffer");
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (dst.impl_ != impl_ || dst.generation_ != impl_->generation)
        return stale_error();
    if (impl_->lost || !impl_->ready)
        return lost_error();
    if (bytes == 0)
        return {};
    if (dst.mapped_ != nullptr) {
        std::memcpy(dst.mapped_, src, bytes);
        return {};
    }
    std::uint64_t smem = 0;
    void* smap = nullptr;
    auto stg = impl_->make_raw_buffer(bytes, true, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, smem, &smap);
    if (!stg)
        return stg.error();
    std::memcpy(smap, src, bytes);
    auto& a = impl_->api;
    a.ResetCommandBuffer(impl_->cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    a.BeginCommandBuffer(impl_->cmd, &bi);
    VkBufferCopy cp{0, 0, bytes};
    a.CmdCopyBuffer(impl_->cmd, from_u64<VkBuffer>(stg.value()), from_u64<VkBuffer>(dst.buffer_), 1, &cp);
    global_barrier(*impl_, impl_->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    a.EndCommandBuffer(impl_->cmd);
    auto st = impl_->submit_and_wait(impl_->cmd, nullptr);
    impl_->free_raw_buffer(stg.value(), smem, true);
    return st;
}

Result<void> Runner::download(const Buffer& src, void* dst, std::size_t bytes) {
    if (bytes > src.size_ || (bytes > 0 && dst == nullptr))
        return make_error(ErrorCode::InvalidArgument, "download range exceeds the buffer");
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (src.impl_ != impl_ || src.generation_ != impl_->generation)
        return stale_error();
    if (impl_->lost || !impl_->ready)
        return lost_error();
    if (bytes == 0)
        return {};
    if (src.mapped_ != nullptr) {
        std::memcpy(dst, src.mapped_, bytes);
        return {};
    }
    std::uint64_t smem = 0;
    void* smap = nullptr;
    auto stg = impl_->make_raw_buffer(bytes, true, VK_BUFFER_USAGE_TRANSFER_DST_BIT, smem, &smap);
    if (!stg)
        return stg.error();
    auto& a = impl_->api;
    a.ResetCommandBuffer(impl_->cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    a.BeginCommandBuffer(impl_->cmd, &bi);
    VkBufferCopy cp{0, 0, bytes};
    a.CmdCopyBuffer(impl_->cmd, from_u64<VkBuffer>(src.buffer_), from_u64<VkBuffer>(stg.value()), 1, &cp);
    global_barrier(*impl_, impl_->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    a.EndCommandBuffer(impl_->cmd);
    auto st = impl_->submit_and_wait(impl_->cmd, nullptr);
    if (st.ok())
        std::memcpy(dst, smap, bytes);
    impl_->free_raw_buffer(stg.value(), smem, true);
    return st;
}

Result<Pipeline> Runner::create_pipeline(const void* spirv, std::size_t spirv_bytes,
                                         std::uint32_t num_buffers, std::uint32_t push_bytes) {
    if (spirv == nullptr || spirv_bytes < 20 || spirv_bytes % 4 != 0)
        return make_error(ErrorCode::InvalidArgument, "SPIR-V blob is empty or not a multiple of 4 bytes");
    std::uint32_t magic = 0;
    std::memcpy(&magic, spirv, 4);
    if (magic != 0x07230203u)
        return make_error(ErrorCode::InvalidArgument, "SPIR-V blob has a bad magic number");
    if (num_buffers == 0 || num_buffers > 8)
        return make_error(ErrorCode::InvalidArgument, "1..8 storage buffers per pipeline are supported");
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->lost || !impl_->ready)
        return lost_error();
    if (push_bytes > impl_->max_push)
        return make_error(ErrorCode::Unsupported, "push constant block larger than the device limit");
    auto& a = impl_->api;
    auto dev = impl_->device;

    std::vector<std::uint32_t> words(spirv_bytes / 4);
    std::memcpy(words.data(), spirv, spirv_bytes);
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = spirv_bytes;
    smi.pCode = words.data();
    VkShaderModule mod = nullptr;
    if (VkResult r = a.CreateShaderModule(dev, &smi, nullptr, &mod); r != VK_SUCCESS)
        return be("vkCreateShaderModule", r);

    std::vector<VkDescriptorSetLayoutBinding> bind(num_buffers);
    for (std::uint32_t i = 0; i < num_buffers; ++i) {
        bind[i] = {};
        bind[i].binding = i;
        bind[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bind[i].descriptorCount = 1;
        bind[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = num_buffers;
    dli.pBindings = bind.data();
    VkDescriptorSetLayout dsl = nullptr;
    if (VkResult r = a.CreateDescriptorSetLayout(dev, &dli, nullptr, &dsl); r != VK_SUCCESS) {
        a.DestroyShaderModule(dev, mod, nullptr);
        return be("vkCreateDescriptorSetLayout", r);
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = push_bytes > 0 ? 1u : 0u;
    pli.pPushConstantRanges = push_bytes > 0 ? &pcr : nullptr;
    VkPipelineLayout pl = nullptr;
    if (VkResult r = a.CreatePipelineLayout(dev, &pli, nullptr, &pl); r != VK_SUCCESS) {
        a.DestroyDescriptorSetLayout(dev, dsl, nullptr);
        a.DestroyShaderModule(dev, mod, nullptr);
        return be("vkCreatePipelineLayout", r);
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    cpi.layout = pl;
    VkPipeline pipe = nullptr;
    if (VkResult r = a.CreateComputePipelines(dev, nullptr, 1, &cpi, nullptr, &pipe); r != VK_SUCCESS) {
        a.DestroyPipelineLayout(dev, pl, nullptr);
        a.DestroyDescriptorSetLayout(dev, dsl, nullptr);
        a.DestroyShaderModule(dev, mod, nullptr);
        return make_error(ErrorCode::Unsupported, "vkCreateComputePipelines failed (VkResult " +
                                                      std::to_string(static_cast<int>(r)) +
                                                      "): the device/driver cannot run this shader");
    }
    Pipeline p;
    p.impl_ = impl_;
    p.module_ = to_u64(mod);
    p.dsl_ = to_u64(dsl);
    p.layout_ = to_u64(pl);
    p.pipeline_ = to_u64(pipe);
    p.num_buffers_ = num_buffers;
    p.push_bytes_ = push_bytes;
    p.generation_ = impl_->generation;
    impl_->live_pipelines[p.pipeline_] = {p.module_, p.dsl_, p.layout_};
    return p;
}

Result<DispatchTiming> Runner::dispatch(const Pipeline& p, std::span<const Buffer* const> buffers,
                                        const void* push, std::uint32_t push_bytes, std::uint32_t gx,
                                        std::uint32_t gy, std::uint32_t gz) {
    if (buffers.size() != p.num_buffers_)
        return make_error(ErrorCode::InvalidArgument, "dispatch buffer count does not match the pipeline");
    if (push_bytes != p.push_bytes_ || (push_bytes > 0 && push == nullptr))
        return make_error(ErrorCode::InvalidArgument,
                          "dispatch push-constant size does not match the pipeline");
    if (gx == 0 || gy == 0 || gz == 0)
        return make_error(ErrorCode::InvalidArgument, "empty dispatch");
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (p.impl_ != impl_ || p.generation_ != impl_->generation)
        return stale_error();
    for (const Buffer* b : buffers)
        if (b == nullptr || b->impl_ != impl_ || b->generation_ != impl_->generation)
            return stale_error();
    if (impl_->lost || !impl_->ready)
        return lost_error();
    auto& a = impl_->api;
    auto dev = impl_->device;

    a.ResetDescriptorPool(dev, impl_->dpool, 0);
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = impl_->dpool;
    dai.descriptorSetCount = 1;
    VkDescriptorSetLayout dsl = from_u64<VkDescriptorSetLayout>(p.dsl_);
    dai.pSetLayouts = &dsl;
    VkDescriptorSet set = nullptr;
    if (VkResult r = a.AllocateDescriptorSets(dev, &dai, &set); r != VK_SUCCESS)
        return be("vkAllocateDescriptorSets", r);
    std::vector<VkDescriptorBufferInfo> infos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        infos[i] = {from_u64<VkBuffer>(buffers[i]->buffer_), 0, VK_WHOLE_SIZE};
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = static_cast<std::uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    a.UpdateDescriptorSets(dev, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    a.ResetCommandBuffer(impl_->cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    a.BeginCommandBuffer(impl_->cmd, &bi);
    const bool ts = impl_->qpool != nullptr;
    if (ts) {
        a.CmdResetQueryPool(impl_->cmd, impl_->qpool, 0, 2);
        a.CmdWriteTimestamp(impl_->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, impl_->qpool, 0);
    }
    a.CmdBindPipeline(impl_->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, from_u64<VkPipeline>(p.pipeline_));
    a.CmdBindDescriptorSets(impl_->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, from_u64<VkPipelineLayout>(p.layout_),
                            0, 1, &set, 0, nullptr);
    if (push_bytes > 0)
        a.CmdPushConstants(impl_->cmd, from_u64<VkPipelineLayout>(p.layout_), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           push_bytes, push);
    a.CmdDispatch(impl_->cmd, gx, gy, gz);
    if (ts)
        a.CmdWriteTimestamp(impl_->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, impl_->qpool, 1);
    global_barrier(
        *impl_, impl_->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    a.EndCommandBuffer(impl_->cmd);

    DispatchTiming t;
    if (auto st = impl_->submit_and_wait(impl_->cmd, &t.wall_ms); !st.ok())
        return st.error();
    if (ts) {
        std::uint64_t q[2] = {0, 0};
        if (a.GetQueryPoolResults(dev, impl_->qpool, 0, 2, sizeof(q), q, sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            const std::uint64_t mask =
                impl_->timestamp_valid_bits >= 64 ? ~0ull : ((1ull << impl_->timestamp_valid_bits) - 1);
            const std::uint64_t d = ((q[1] & mask) - (q[0] & mask)) & mask;
            t.gpu_ms = static_cast<double>(d) * static_cast<double>(impl_->timestamp_period_ns) / 1e6;
        }
    }
    return t;
}

Result<void> Runner::reset() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->teardown();
    return impl_->init();
}

}  // namespace hcs::kernels::vulkan
