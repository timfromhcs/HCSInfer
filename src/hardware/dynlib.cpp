#include "hcs/hardware/dynlib.hpp"

#ifdef _WIN32
#include "win_util.hpp"
#else
#include <dlfcn.h>
#endif

namespace hcs::hardware {

const char* vulkan_loader_name() noexcept {
#if defined(_WIN32)
    return "vulkan-1.dll";
#elif defined(__APPLE__)
    return "libvulkan.1.dylib";
#else
    return "libvulkan.so.1";
#endif
}

namespace {

VulkanLoader open_vulkan_loader() {
    VulkanLoader l;
#ifdef _WIN32
    l.tried = "vulkan-1.dll";
    HMODULE lib = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib)
        return l;
    l.handle = lib;
    l.get_instance_proc_addr =
        reinterpret_cast<void*>(reinterpret_cast<void (*)()>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(lib, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        l.path = win::narrow(std::wstring(path, n));
#else
#if defined(__APPLE__)
    const char* const names[] = {"libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib"};
#else
    const char* const names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (const char* n : names) {
        if (!l.tried.empty())
            l.tried += ", ";
        l.tried += n;
        void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            l.handle = h;
            break;
        }
    }
    if (!l.handle)
        return l;
    l.get_instance_proc_addr = dlsym(l.handle, "vkGetInstanceProcAddr");
    if (l.get_instance_proc_addr) {
        Dl_info info{};
        if (dladdr(l.get_instance_proc_addr, &info) != 0 && info.dli_fname)
            l.path = info.dli_fname;
    }
#endif
    return l;
}

}  // namespace

const VulkanLoader& vulkan_loader() {
    static const VulkanLoader loader = open_vulkan_loader();
    return loader;
}

}  // namespace hcs::hardware
