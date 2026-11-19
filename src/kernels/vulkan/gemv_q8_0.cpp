// HCSInfer experimental Vulkan Q8_0 GEMV kernel (AGENTS.md M8).
//
// Self-contained: vulkan-1.dll is loaded dynamically (VK_NO_PROTOTYPES, no import library), the SPIR-V is
// embedded at build time, and the buffers are host-visible so there is no staging step in this first version.
// The kernel is off by default and never on the model load path; see docs/vulkan-kernels.md.
#include "hcs/kernels/vulkan.hpp"

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hcs/kernels/cpu.hpp"
#include "hcs/shaders/GemvQ8_0.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hcs::kernels::vulkan {

namespace {

std::atomic<bool> g_enabled{false};

const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return "integrated_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return "discrete_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return "virtual_gpu";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return "cpu";
        default:
            return "other";
    }
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty())
        return true;
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), lower);
    std::transform(n.begin(), n.end(), n.begin(), lower);
    return h.find(n) != std::string::npos;
}

// --- dynamic loader -----------------------------------------------------------------------------
struct VkApi {
    HMODULE lib = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkEnumeratePhysicalDevices enum_devices = nullptr;
    PFN_vkGetPhysicalDeviceProperties get_props = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_props = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props = nullptr;
    PFN_vkCreateDevice create_device = nullptr;
    PFN_vkGetDeviceQueue get_queue = nullptr;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkDestroyDevice destroy_device = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements get_buffer_reqs = nullptr;
    PFN_vkAllocateMemory alloc_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkBindBufferMemory bind_buffer = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    PFN_vkUnmapMemory unmap_memory = nullptr;
    PFN_vkCreateShaderModule create_shader = nullptr;
    PFN_vkDestroyShaderModule destroy_shader = nullptr;
    PFN_vkCreateDescriptorSetLayout create_dsl = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_dsl = nullptr;
    PFN_vkCreateDescriptorPool create_pool = nullptr;
    PFN_vkDestroyDescriptorPool destroy_pool = nullptr;
    PFN_vkResetDescriptorPool reset_pool = nullptr;
    PFN_vkAllocateDescriptorSets alloc_sets = nullptr;
    PFN_vkUpdateDescriptorSets update_sets = nullptr;
    PFN_vkCreatePipelineLayout create_pl = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pl = nullptr;
    PFN_vkCreateComputePipelines create_pipeline = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkCreateCommandPool create_cmd_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_cmd_pool = nullptr;
    PFN_vkResetCommandPool reset_cmd_pool = nullptr;
    PFN_vkAllocateCommandBuffers alloc_cmd = nullptr;
    PFN_vkBeginCommandBuffer begin_cmd = nullptr;
    PFN_vkEndCommandBuffer end_cmd = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmd_bind_sets = nullptr;
    PFN_vkCmdPushConstants cmd_push = nullptr;
    PFN_vkCmdDispatch cmd_dispatch = nullptr;
    PFN_vkCmdPipelineBarrier cmd_barrier = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkResetFences reset_fence = nullptr;
    PFN_vkWaitForFences wait_fence = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
};

template <typename Fn>
Fn load_inst(VkApi& a, VkInstance inst, const char* name) {
    return reinterpret_cast<Fn>(a.gipa(inst, name));
}

VkApi& api() {
    static VkApi a;
    static std::once_flag once;
    std::call_once(once, [&] {
#ifdef _WIN32
        a.lib = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!a.lib)
            return;
        a.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            reinterpret_cast<void*>(GetProcAddress(a.lib, "vkGetInstanceProcAddr")));
#endif
        if (!a.gipa)
            return;
        a.create_instance = load_inst<PFN_vkCreateInstance>(a, nullptr, "vkCreateInstance");
    });
    return a;
}

// --- cached device context ----------------------------------------------------------------------
struct Ctx {
    bool ready = false;
    std::string name_substr;
    DeviceInfo info;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

std::mutex& ctx_mutex() {
    static std::mutex m;
    return m;
}

// --- small helpers ------------------------------------------------------------------------------
Result<std::uint32_t> find_memory_type(const Ctx& c, std::uint32_t bits, VkMemoryPropertyFlags props) {
    VkApi& a = api();
    VkPhysicalDeviceMemoryProperties mp{};
    a.get_mem_props(c.physical, &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return make_error(ErrorCode::BackendError,
                      "no HOST_VISIBLE|HOST_COHERENT memory type for a kernel buffer");
}

struct Buf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

// A host-visible, host-coherent storage buffer that is mapped for the whole call (no staging in v1).
Result<Buf> make_buffer(const Ctx& c, VkDeviceSize size) {
    VkApi& a = api();
    Buf b;
    b.size = size;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (VkResult r = a.create_buffer(c.device, &bi, nullptr, &b.buffer); r != VK_SUCCESS)
        return make_error(ErrorCode::BackendError, "vkCreateBuffer failed (" + std::to_string(r) + ")");
    VkMemoryRequirements req{};
    a.get_buffer_reqs(c.device, b.buffer, &req);
    auto mt = find_memory_type(c, req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!mt) {
        a.destroy_buffer(c.device, b.buffer, nullptr);
        return mt.error();
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt.value();
    if (VkResult r = a.alloc_memory(c.device, &ai, nullptr, &b.memory); r != VK_SUCCESS) {
        a.destroy_buffer(c.device, b.buffer, nullptr);
        return make_error(ErrorCode::BackendError, "vkAllocateMemory failed (" + std::to_string(r) + ")");
    }
    if (VkResult r = a.bind_buffer(c.device, b.buffer, b.memory, 0); r != VK_SUCCESS) {
        a.free_memory(c.device, b.memory, nullptr);
        a.destroy_buffer(c.device, b.buffer, nullptr);
        return make_error(ErrorCode::BackendError, "vkBindBufferMemory failed (" + std::to_string(r) + ")");
    }
    if (VkResult r = a.map_memory(c.device, b.memory, 0, size, 0, &b.mapped); r != VK_SUCCESS) {
        a.free_memory(c.device, b.memory, nullptr);
        a.destroy_buffer(c.device, b.buffer, nullptr);
        return make_error(ErrorCode::BackendError, "vkMapMemory failed (" + std::to_string(r) + ")");
    }
    return b;
}

void destroy_buffer(const Ctx& c, Buf& b) {
    VkApi& a = api();
    if (b.mapped) {
        a.unmap_memory(c.device, b.memory);
        b.mapped = nullptr;
    }
    if (b.buffer)
        a.destroy_buffer(c.device, b.buffer, nullptr);
    if (b.memory)
        a.free_memory(c.device, b.memory, nullptr);
    b.buffer = VK_NULL_HANDLE;
    b.memory = VK_NULL_HANDLE;
    b.size = 0;
}

void destroy_context(Ctx& c) {
    VkApi& a = api();
    if (!a.gipa)
        return;
    if (c.device) {
        a.device_wait_idle(c.device);
        if (c.fence)
            a.destroy_fence(c.device, c.fence, nullptr);
        if (c.cmd_pool)
            a.destroy_cmd_pool(c.device, c.cmd_pool, nullptr);
        if (c.pipeline)
            a.destroy_pipeline(c.device, c.pipeline, nullptr);
        if (c.shader)
            a.destroy_shader(c.device, c.shader, nullptr);
        if (c.playout)
            a.destroy_pl(c.device, c.playout, nullptr);
        if (c.dpool)
            a.destroy_pool(c.device, c.dpool, nullptr);
        if (c.dsl)
            a.destroy_dsl(c.device, c.dsl, nullptr);
        a.destroy_device(c.device, nullptr);
    }
    if (c.instance && a.destroy_instance)
        a.destroy_instance(c.instance, nullptr);
    c = Ctx{};
}

Result<Ctx*> create_context(const std::string& substr) {
    VkApi& a = api();
    if (!a.gipa || !a.create_instance)
        return make_error(ErrorCode::Unsupported, "vulkan-1.dll or vkGetInstanceProcAddr is not available");

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "HCSInfer kernel";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    Ctx& c = ctx();
    if (VkResult r = a.create_instance(&ici, nullptr, &c.instance); r != VK_SUCCESS)
        return make_error(ErrorCode::BackendError, "vkCreateInstance failed (" + std::to_string(r) + ")");

    a.enum_devices = load_inst<PFN_vkEnumeratePhysicalDevices>(a, c.instance, "vkEnumeratePhysicalDevices");
    a.get_props =
        load_inst<PFN_vkGetPhysicalDeviceProperties>(a, c.instance, "vkGetPhysicalDeviceProperties");
    a.get_queue_props = load_inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        a, c.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    a.get_mem_props = load_inst<PFN_vkGetPhysicalDeviceMemoryProperties>(
        a, c.instance, "vkGetPhysicalDeviceMemoryProperties");
    a.create_device = load_inst<PFN_vkCreateDevice>(a, c.instance, "vkCreateDevice");
    a.destroy_instance = load_inst<PFN_vkDestroyInstance>(a, c.instance, "vkDestroyInstance");
    if (!a.enum_devices || !a.get_props || !a.get_queue_props || !a.create_device) {
        destroy_context(c);
        return make_error(ErrorCode::Unsupported, "required Vulkan instance functions are missing");
    }

    std::uint32_t ndev = 0;
    a.enum_devices(c.instance, &ndev, nullptr);
    std::vector<VkPhysicalDevice> devs(ndev);
    a.enum_devices(c.instance, &ndev, devs.data());

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    DeviceInfo info;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p{};
        a.get_props(d, &p);
        std::uint32_t nq = 0;
        a.get_queue_props(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        a.get_queue_props(d, &nq, qs.data());
        int fam = -1;
        for (std::uint32_t i = 0; i < nq; ++i) {
            if ((qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                fam = static_cast<int>(i);
                break;
            }
        }
        if (fam < 0)
            continue;
        const std::string name(p.deviceName);
        if (!contains_ci(name, substr))
            continue;
        chosen = d;
        family = static_cast<std::uint32_t>(fam);
        info.name = name;
        info.vendor_id = p.vendorID;
        info.device_id = p.deviceID;
        info.api_version = p.apiVersion;
        info.device_type = device_type_name(p.deviceType);
        break;
    }
    if (chosen == VK_NULL_HANDLE) {
        destroy_context(c);
        return make_error(ErrorCode::Unsupported,
                          "no Vulkan device with a compute queue matches '" + substr + "'");
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (VkResult r = a.create_device(chosen, &dci, nullptr, &c.device); r != VK_SUCCESS) {
        destroy_context(c);
        return make_error(ErrorCode::BackendError, "vkCreateDevice failed (" + std::to_string(r) + ")");
    }
    c.physical = chosen;
    c.queue_family = family;

#define HCS_LOAD_DEV(member, name) a.member = load_inst<PFN_##name>(a, c.instance, #name)
    HCS_LOAD_DEV(get_queue, vkGetDeviceQueue);
    HCS_LOAD_DEV(destroy_device, vkDestroyDevice);
    HCS_LOAD_DEV(device_wait_idle, vkDeviceWaitIdle);
    HCS_LOAD_DEV(create_buffer, vkCreateBuffer);
    HCS_LOAD_DEV(destroy_buffer, vkDestroyBuffer);
    HCS_LOAD_DEV(get_buffer_reqs, vkGetBufferMemoryRequirements);
    HCS_LOAD_DEV(alloc_memory, vkAllocateMemory);
    HCS_LOAD_DEV(free_memory, vkFreeMemory);
    HCS_LOAD_DEV(bind_buffer, vkBindBufferMemory);
    HCS_LOAD_DEV(map_memory, vkMapMemory);
    HCS_LOAD_DEV(unmap_memory, vkUnmapMemory);
    HCS_LOAD_DEV(create_shader, vkCreateShaderModule);
    HCS_LOAD_DEV(destroy_shader, vkDestroyShaderModule);
    HCS_LOAD_DEV(create_dsl, vkCreateDescriptorSetLayout);
    HCS_LOAD_DEV(destroy_dsl, vkDestroyDescriptorSetLayout);
    HCS_LOAD_DEV(create_pool, vkCreateDescriptorPool);
    HCS_LOAD_DEV(destroy_pool, vkDestroyDescriptorPool);
    HCS_LOAD_DEV(reset_pool, vkResetDescriptorPool);
    HCS_LOAD_DEV(alloc_sets, vkAllocateDescriptorSets);
    HCS_LOAD_DEV(update_sets, vkUpdateDescriptorSets);
    HCS_LOAD_DEV(create_pl, vkCreatePipelineLayout);
    HCS_LOAD_DEV(destroy_pl, vkDestroyPipelineLayout);
    HCS_LOAD_DEV(create_pipeline, vkCreateComputePipelines);
    HCS_LOAD_DEV(destroy_pipeline, vkDestroyPipeline);
    HCS_LOAD_DEV(create_cmd_pool, vkCreateCommandPool);
    HCS_LOAD_DEV(destroy_cmd_pool, vkDestroyCommandPool);
    HCS_LOAD_DEV(reset_cmd_pool, vkResetCommandPool);
    HCS_LOAD_DEV(alloc_cmd, vkAllocateCommandBuffers);
    HCS_LOAD_DEV(begin_cmd, vkBeginCommandBuffer);
    HCS_LOAD_DEV(end_cmd, vkEndCommandBuffer);
    HCS_LOAD_DEV(cmd_bind_pipeline, vkCmdBindPipeline);
    HCS_LOAD_DEV(cmd_bind_sets, vkCmdBindDescriptorSets);
    HCS_LOAD_DEV(cmd_push, vkCmdPushConstants);
    HCS_LOAD_DEV(cmd_dispatch, vkCmdDispatch);
    HCS_LOAD_DEV(cmd_barrier, vkCmdPipelineBarrier);
    HCS_LOAD_DEV(create_fence, vkCreateFence);
    HCS_LOAD_DEV(destroy_fence, vkDestroyFence);
    HCS_LOAD_DEV(reset_fence, vkResetFences);
    HCS_LOAD_DEV(wait_fence, vkWaitForFences);
    HCS_LOAD_DEV(queue_submit, vkQueueSubmit);
    HCS_LOAD_DEV(queue_wait_idle, vkQueueWaitIdle);
#undef HCS_LOAD_DEV
    if (!a.get_queue || !a.create_pipeline || !a.queue_submit || !a.wait_fence) {
        destroy_context(c);
        return make_error(ErrorCode::Unsupported, "required Vulkan device functions are missing");
    }
    a.get_queue(c.device, family, 0, &c.queue);

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 3;
    dli.pBindings = bindings;
    if (VkResult r = a.create_dsl(c.device, &dli, nullptr, &c.dsl); r != VK_SUCCESS) {
        destroy_context(c);
        return make_error(ErrorCode::BackendError, "vkCreateDescriptorSetLayout failed");
    }
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 3;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (VkResult r = a.create_pool(c.device, &dpi, nullptr, &c.dpool); r != VK_SUCCESS) {
        destroy_context(c);
        return make_error(ErrorCode::BackendError, "vkCreateDescriptorPool failed");
    }
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 2 * sizeof(std::uint32_t);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &c.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (VkResult r = a.create_pl(c.device, &pli, nullptr, &c.playout); r != VK_SUCCESS) {
        destroy_context(c);
        return make_error(ErrorCode::BackendError, "vkCreatePipelineLayout failed");
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = hcs::shaders::kGemvQ8_0Size;
    smi.pCode = reinterpret_cast<const std::uint32_t*>(hcs::shaders::kGemvQ8_0);
    if (VkResult r = a.create_shader(c.device, &smi, nullptr, &c.shader); r != VK_SUCCESS) {
        destroy_context(c);
        return make_error(ErrorCode::BackendError, "vkCreateShaderModule failed");
    }
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = c.shader;
    stage.pName = "main";
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage = stage;
    cpi.layout = c.playout;
    if (VkResult r = a.create_pipeline(c.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &c.pipeline);
        r != VK_SUCCESS) {
        destroy_context(c);
        return make_error(ErrorCode::BackendError,
                          "vkCreateComputePipelines failed (" + std::to_string(r) + ")");
    }
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    if (VkResult r = a.create_cmd_pool(c.device, &cpci, nullptr, &c.cmd_pool); r != VK_SUCCESS) {
        destroy_context(c);
        return make_error(ErrorCode::BackendError, "vkCreateCommandPool failed");
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (VkResult r = a.create_fence(c.device, &fci, nullptr, &c.fence); r != VK_SUCCESS) {
        destroy_context(c);
        return make_error(ErrorCode::BackendError, "vkCreateFence failed");
    }
    c.info = std::move(info);
    c.name_substr = substr;
    c.ready = true;
    return &c;
}

Result<Ctx*> ensure_context(const std::string& substr) {
    Ctx& c = ctx();
    if (c.ready && c.name_substr == substr)
        return &c;
    if (c.ready)
        destroy_context(c);
    return create_context(substr);
}

Result<void> run_gemv(const GemvRequest& req) {
    VkApi& a = api();
    Ctx* cp = nullptr;
    {
        auto r = ensure_context(std::string{});
        if (!r)
            return r.error();
        cp = r.value();
    }
    Ctx& c = *cp;

    auto xb = make_buffer(c, static_cast<VkDeviceSize>(req.k) * 4);
    if (!xb)
        return xb.error();
    Buf x = xb.value();
    auto wb = make_buffer(c, static_cast<VkDeviceSize>(cpu::q8_0_bytes(req.m, req.k)));
    if (!wb) {
        destroy_buffer(c, x);
        return wb.error();
    }
    Buf w = wb.value();
    auto yb = make_buffer(c, static_cast<VkDeviceSize>(req.m) * 4);
    if (!yb) {
        destroy_buffer(c, x);
        destroy_buffer(c, w);
        return yb.error();
    }
    Buf y = yb.value();
    std::memcpy(x.mapped, req.x, static_cast<std::size_t>(req.k) * 4);
    std::memcpy(w.mapped, req.w_q8_0, static_cast<std::size_t>(cpu::q8_0_bytes(req.m, req.k)));

    Result<void> result{};
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    do {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = c.dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &c.dsl;
        if (VkResult r = a.alloc_sets(c.device, &dsai, &set); r != VK_SUCCESS) {
            result = make_error(ErrorCode::BackendError, "vkAllocateDescriptorSets failed");
            break;
        }
        VkDescriptorBufferInfo bi[3] = {
            {x.buffer, 0, VK_WHOLE_SIZE}, {w.buffer, 0, VK_WHOLE_SIZE}, {y.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet wr[3]{};
        for (int i = 0; i < 3; ++i) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = set;
            wr[i].dstBinding = static_cast<std::uint32_t>(i);
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr[i].pBufferInfo = &bi[i];
        }
        a.update_sets(c.device, 3, wr, 0, nullptr);

        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = c.cmd_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (VkResult r = a.alloc_cmd(c.device, &cai, &cmd); r != VK_SUCCESS) {
            result = make_error(ErrorCode::BackendError, "vkAllocateCommandBuffers failed");
            break;
        }
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        a.begin_cmd(cmd, &cbbi);
        a.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
        a.cmd_bind_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.playout, 0, 1, &set, 0, nullptr);
        std::uint32_t pc[2] = {req.m, req.k};
        a.cmd_push(cmd, c.playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        const std::uint32_t groups = (req.m + 63u) / 64u;
        a.cmd_dispatch(cmd, groups, 1, 1);
        VkBufferMemoryBarrier bmb{};
        bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = y.buffer;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        a.cmd_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                      &bmb, 0, nullptr);
        if (VkResult r = a.end_cmd(cmd); r != VK_SUCCESS) {
            result = make_error(ErrorCode::BackendError, "vkEndCommandBuffer failed");
            break;
        }
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (VkResult r = a.reset_fence(c.device, 1, &c.fence); r != VK_SUCCESS) {
            result = make_error(ErrorCode::BackendError, "vkResetFences failed");
            break;
        }
        if (VkResult r = a.queue_submit(c.queue, 1, &si, c.fence); r != VK_SUCCESS) {
            result = make_error(ErrorCode::BackendError, "vkQueueSubmit failed (" + std::to_string(r) + ")");
            break;
        }
        if (VkResult r = a.wait_fence(c.device, 1, &c.fence, VK_TRUE, 60ull * 1000 * 1000 * 1000);
            r != VK_SUCCESS) {
            result = make_error(ErrorCode::BackendError,
                                "vkWaitForFences failed / timed out (" + std::to_string(r) + ")");
            break;
        }
        std::memcpy(req.y, y.mapped, static_cast<std::size_t>(req.m) * 4);
    } while (false);

    if (cmd)
        a.reset_cmd_pool(c.device, c.cmd_pool, 0);
    if (set)
        a.reset_pool(c.device, c.dpool, 0);
    destroy_buffer(c, x);
    destroy_buffer(c, w);
    destroy_buffer(c, y);
    return result;
}

}  // namespace

bool enabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}
void set_enabled(bool on) noexcept {
    g_enabled.store(on, std::memory_order_relaxed);
}

bool available() noexcept {
    std::lock_guard lk(ctx_mutex());
    return static_cast<bool>(ensure_context(std::string{}));
}

Result<DeviceInfo> device_info(std::string_view device_name_substr) {
    std::lock_guard lk(ctx_mutex());
    auto r = ensure_context(std::string(device_name_substr));
    if (!r)
        return r.error();
    return r.value()->info;
}

Result<void> gemv_q8_0(const GemvRequest& req, std::string_view device_name_substr) {
    if (!enabled())
        return make_error(ErrorCode::Unsupported, "the HCS Vulkan kernels are disabled (feature flag off)");
    if (req.x == nullptr || req.w_q8_0 == nullptr || req.y == nullptr)
        return make_error(ErrorCode::InvalidArgument, "null buffer in the Q8_0 GEMV request");
    if (req.m == 0)
        return make_error(ErrorCode::InvalidArgument, "m must be > 0");
    if (req.k == 0 || req.k % cpu::kQ8_0BlockValues != 0)
        return make_error(ErrorCode::InvalidArgument, "k must be a positive multiple of 32");
    std::lock_guard lk(ctx_mutex());
    (void)device_name_substr;  // the creation path selects the default device; device_info() selects by name
    return run_gemv(req);
}

void shutdown() noexcept {
    std::lock_guard lk(ctx_mutex());
    destroy_context(ctx());
}

}  // namespace hcs::kernels::vulkan
