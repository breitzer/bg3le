// The exported half of bg3le's Detours stand-in, for the Vulkan entry points
// bg3se's ImGui overlay wraps.
//
// See src/detour_interpose.cpp for the other half. In short: bg3se registers
// a replacement against a target function, and on Linux the way to make the
// game's call land on that replacement is to export the symbol here and let
// the dynamic linker route it.
//
// The pairs are registered with the shim up front, and that is the part that
// took a stack overflow to get right. bg3se's EnableHooks does
//
//     auto f = vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
//     CreateInstanceHook_.Wrap(ResolveFunctionTrampoline(f));
//
// and because src/vulkan_memory.cpp diverts these names here, the pointer it
// registers against is a forwarder, not the loader's function. The first
// version handed that same pointer back as "the original to call through", so
// bg3se's post-hook called the forwarder, which found the hook, which called
// the post-hook, until the stack ran out -- 0x7f..fac in vkCreateInstance
// under two hundred frames of StaticPostHook.
//
// So every forwarder tells the shim which function it stands in for, before
// any hook is registered, and DetourAttachEx records and trampolines against
// the real one.
//
// On, as upstream's is; BG3LE_IMGUI=0 turns it off. vkQueuePresentKHR runs
// every frame and the overlay does real Vulkan work inside it.
//
// The hooked functions are bg3se's, by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); the forwarding is ours.

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "detour_interpose.h"
#include "log.h"

namespace bg3le {

void imgui_overlay_start();
void vulkan_register_forwarders();

bool imgui_overlay_wanted() {
    static const bool on = [] {
        const char* opt = std::getenv("BG3LE_IMGUI");
        return opt == nullptr || opt[0] != '0';
    }();
    return on;
}

}  // namespace bg3le

namespace {

// The loader's own function, past ours. Same reasoning as
// src/vulkan_memory.cpp: RTLD_NEXT alone misses if nothing has pulled the
// loader in yet, and a null here would turn a forwarder into a crash.
void* loader() {
    static void* handle = [] () -> void* {
        void* h = ::dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (h == nullptr) h = ::dlopen("libvulkan.so.1", RTLD_LAZY);
        return h;
    }();
    return handle;
}

template <typename Fn>
Fn real(char const* name) {
    if (void* next = ::dlsym(RTLD_NEXT, name)) return (Fn)next;
    if (void* h = loader()) {
        if (void* found = ::dlsym(h, name)) return (Fn)found;
    }
    return nullptr;
}

// The replacement registered against the loader's function, or null.
//
// One key, because DetourAttachEx resolves a forwarder to the function behind
// it before recording -- see detour_register_forwarder for why that matters.
template <typename Fn>
Fn hooked(Fn original) {
    if (original == nullptr || !bg3le::imgui_overlay_wanted()) return nullptr;
    return (Fn)bg3le::detour_for((void const*)original);
}

}  // namespace

// One forwarder per wrapped entry point. Each resolves the loader's function,
// asks whether a replacement is registered against it, and calls whichever it
// should.
#define BG3LE_FORWARD(ret, name, params, args)                                \
    extern "C" ret name params {                                              \
        using Fn = ret (*) params;                                            \
        static const Fn next = real<Fn>(#name);                               \
        const Fn take = hooked<Fn>(next);                                     \
        if (take != nullptr && take != (Fn)&name) return take args;           \
        return next args;                                                     \
    }

// The one forwarder that is not written by the macro: the overlay has to be
// built here, after the engine heap exists and before the instance does, and
// the hook lookup has to happen after that or there is nothing to find.
extern "C" VkResult vkCreateInstance(VkInstanceCreateInfo const* info,
                                     VkAllocationCallbacks const* alloc,
                                     VkInstance* out) {
    using Fn = VkResult (*)(VkInstanceCreateInfo const*,
                            VkAllocationCallbacks const*, VkInstance*);
    static const Fn next = real<Fn>("vkCreateInstance");

    bg3le::vulkan_register_forwarders();
    bg3le::imgui_overlay_start();

    const Fn take = hooked<Fn>(next);
    if (take != nullptr && take != (Fn)&vkCreateInstance) {
        return take(info, alloc, out);
    }
    return next(info, alloc, out);
}

// The physical device the game renders with, for the swapchain check below.
static VkPhysicalDevice g_physical = VK_NULL_HANDLE;

namespace {

// The driver, the Vulkan layers and the display environment, once.
void log_environment(VkPhysicalDevice phys) {
    using Props2 = void (*)(VkPhysicalDevice, VkPhysicalDeviceProperties2*);
    using Layers = VkResult (*)(uint32_t*, VkLayerProperties*);
    static const Props2 props = real<Props2>("vkGetPhysicalDeviceProperties2");
    static const Layers layers = real<Layers>("vkEnumerateInstanceLayerProperties");

    if (props != nullptr) {
        VkPhysicalDeviceDriverProperties driver{};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p{};
        p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p.pNext = &driver;
        props(phys, &p);
        bg3le::logf("vulkan: %s, Vulkan %u.%u.%u, driver %s (%s)", p.properties.deviceName,
                    VK_API_VERSION_MAJOR(p.properties.apiVersion), VK_API_VERSION_MINOR(p.properties.apiVersion),
                    VK_API_VERSION_PATCH(p.properties.apiVersion), driver.driverName, driver.driverInfo);
    }
    uint32_t n = 0;
    if (layers != nullptr && layers(&n, nullptr) == VK_SUCCESS && n > 0) {
        std::unique_ptr<VkLayerProperties[]> all(new VkLayerProperties[n]);
        if (layers(&n, all.get()) == VK_SUCCESS) {
            std::string names;
            for (uint32_t i = 0; i < n; ++i) names += (i ? ", " : "") + std::string(all[i].layerName);
            bg3le::logf("vulkan: layers available: %s", names.c_str());
        }
    }
    char const* const vars[] = {
        "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "DISPLAY", "WAYLAND_DISPLAY",
        "SDL_VIDEODRIVER", "GAMESCOPE_WAYLAND_DISPLAY", "ENABLE_GAMESCOPE_WSI",
        "SteamDeck", "SteamGamepadUI", "SteamOS", "VK_INSTANCE_LAYERS",
        "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE", "MESA_VK_WSI_PRESENT_MODE",
        "ENABLE_VKBASALT", "MANGOHUD", "LD_PRELOAD",
    };
    for (char const* name : vars) {
        if (char const* v = std::getenv(name)) bg3le::logf("env: %s=%s", name, v);
    }
}

// System memory, and the device's heaps against their budgets.
void log_memory(VkPhysicalDevice phys) {
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[128];
        std::string out;
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            char key[32];
            unsigned long long kb = 0;
            if (std::sscanf(line, "%31[^:]: %llu", key, &kb) != 2) continue;
            if (std::strcmp(key, "MemTotal") && std::strcmp(key, "MemAvailable")
                && std::strcmp(key, "SwapTotal") && std::strcmp(key, "SwapFree")) continue;
            if (!out.empty()) out += ", ";
            out += std::string(key) + " " + std::to_string(kb >> 10) + " MB";
        }
        std::fclose(f);
        bg3le::logf("memory: %s", out.c_str());
    }

    using Mem2 = void (*)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2*);
    static const Mem2 mem = real<Mem2>("vkGetPhysicalDeviceMemoryProperties2");
    if (mem == nullptr || phys == VK_NULL_HANDLE) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 p{};
    p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    p.pNext = &budget;
    mem(phys, &p);
    for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; ++i) {
        auto const& heap = p.memoryProperties.memoryHeaps[i];
        bg3le::logf("memory: heap %u%s: %llu of %llu MB used, budget %llu MB", i,
                    (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device-local)" : "",
                    (unsigned long long)(budget.heapUsage[i] >> 20), (unsigned long long)(heap.size >> 20),
                    (unsigned long long)(budget.heapBudget[i] >> 20));
    }
}

// What the surface still offers, after a creation failed.
void log_surface(VkPhysicalDevice phys, VkSurfaceKHR surface) {
    using Modes = VkResult (*)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*);
    using Formats = VkResult (*)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*);
    static const Modes modes = real<Modes>("vkGetPhysicalDeviceSurfacePresentModesKHR");
    static const Formats formats = real<Formats>("vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (phys == VK_NULL_HANDLE) return;

    std::string out;
    if (modes != nullptr) {
        VkPresentModeKHR list[16];
        uint32_t n = 16;
        const VkResult r = modes(phys, surface, &n, list);
        out += "present modes (" + std::to_string((int)r) + ")";
        for (uint32_t i = 0; r >= 0 && i < n; ++i) out += " " + std::to_string((int)list[i]);
    }
    if (formats != nullptr) {
        VkSurfaceFormatKHR list[32];
        uint32_t n = 32;
        const VkResult r = formats(phys, surface, &n, list);
        out += "; formats (" + std::to_string((int)r) + ")";
        for (uint32_t i = 0; r >= 0 && i < n; ++i) {
            out += " " + std::to_string((int)list[i].format) + "/" + std::to_string((int)list[i].colorSpace);
        }
    }
    bg3le::logf("vulkan: after the failure, the surface offers %s", out.c_str());
}

}  // namespace

extern "C" VkResult vkCreateDevice(VkPhysicalDevice phys,
                                   VkDeviceCreateInfo const* info,
                                   VkAllocationCallbacks const* alloc,
                                   VkDevice* out) {
    using Fn = VkResult (*)(VkPhysicalDevice, VkDeviceCreateInfo const*,
                            VkAllocationCallbacks const*, VkDevice*);
    static const Fn next = real<Fn>("vkCreateDevice");
    // The first is the game's; a layer such as lsfg-vk may make its own later.
    if (g_physical == VK_NULL_HANDLE) {
        g_physical = phys;
        log_environment(phys);
    }
    const Fn take = hooked<Fn>(next);
    if (take != nullptr && take != (Fn)&vkCreateDevice) return take(phys, info, alloc, out);
    return next(phys, info, alloc, out);
}

BG3LE_FORWARD(void, vkDestroyDevice,
              (VkDevice dev, VkAllocationCallbacks const* alloc),
              (dev, alloc))

BG3LE_FORWARD(VkResult, vkCreatePipelineCache,
              (VkDevice dev, VkPipelineCacheCreateInfo const* info,
               VkAllocationCallbacks const* alloc, VkPipelineCache* out),
              (dev, info, alloc, out))

// An HDR swapchain is also made copyable, which the overlay's compositor
// needs to read the game's frame (src/vendor/imgui_hdr.cpp).
extern "C" VkResult vkCreateSwapchainKHR(VkDevice dev,
                                         VkSwapchainCreateInfoKHR const* info,
                                         VkAllocationCallbacks const* alloc,
                                         VkSwapchainKHR* out) {
    using Fn = VkResult (*)(VkDevice, VkSwapchainCreateInfoKHR const*,
                            VkAllocationCallbacks const*, VkSwapchainKHR*);
    static const Fn next = real<Fn>("vkCreateSwapchainKHR");
    using Caps = VkResult (*)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*);
    static const Caps caps = real<Caps>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    VkSwapchainCreateInfoKHR copy = *info;
    const bool hdr = info->imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT
        || info->imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    if (hdr && bg3le::imgui_overlay_wanted() && caps != nullptr
        && g_physical != VK_NULL_HANDLE
        && (info->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        VkSurfaceCapabilitiesKHR have{};
        if (caps(g_physical, info->surface, &have) == VK_SUCCESS
            && (have.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
            copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
    }

    bg3le::logf("vulkan: creating a swapchain, %ux%u format %d colour space %d usage %#x (asked %#x), "
                "present mode %d, %u images, old %p (%s)",
                copy.imageExtent.width, copy.imageExtent.height, (int)copy.imageFormat,
                (int)copy.imageColorSpace, (unsigned)copy.imageUsage, (unsigned)info->imageUsage,
                (int)copy.presentMode, copy.minImageCount, (void*)copy.oldSwapchain, bg3le::thread_tag());
    // What the surface wants, against what the game asked for.
    VkSurfaceCapabilitiesKHR surface{};
    const VkResult got = caps != nullptr && g_physical != VK_NULL_HANDLE
        ? caps(g_physical, info->surface, &surface) : VK_ERROR_INITIALIZATION_FAILED;
    if (got == VK_SUCCESS) {
        bg3le::logf("vulkan: the surface is %ux%u, extents %ux%u to %ux%u, %u to %u images",
                    surface.currentExtent.width, surface.currentExtent.height,
                    surface.minImageExtent.width, surface.minImageExtent.height,
                    surface.maxImageExtent.width, surface.maxImageExtent.height,
                    surface.minImageCount, surface.maxImageCount);
    } else {
        bg3le::logf("vulkan: the surface's capabilities could not be read (%d)", (int)got);
    }
    log_memory(g_physical);
    const Fn take = hooked<Fn>(next);
    const VkResult result = (take != nullptr && take != (Fn)&vkCreateSwapchainKHR)
        ? take(dev, &copy, alloc, out)
        : next(dev, &copy, alloc, out);
    bg3le::logf("vulkan: swapchain created: %d", (int)result);
    if (result != VK_SUCCESS) {
        log_surface(g_physical, info->surface);
        log_memory(g_physical);
    }
    return result;
}

// Every live image view, so the overlay can refuse a texture handle that is
// not one: a misread or destroyed view crashes the driver (NVIDIA, in
// AddTexture, opening MCM). Not hooked by bg3se, so no detour lookup.
namespace {
std::mutex g_views_lock;
std::unordered_set<VkImageView> g_views;
// Views seen destroyed and not created again, to tell a destroy from a missed creation.
std::unordered_set<VkImageView> g_destroyed_views;
std::atomic<bool> g_views_tracked{false};
std::atomic<unsigned> g_unseen_destroys{0};

}  // namespace

extern "C" VkResult vkCreateImageView(VkDevice dev,
                                      VkImageViewCreateInfo const* info,
                                      VkAllocationCallbacks const* alloc,
                                      VkImageView* out) {
    using Fn = VkResult (*)(VkDevice, VkImageViewCreateInfo const*,
                            VkAllocationCallbacks const*, VkImageView*);
    static const Fn next = real<Fn>("vkCreateImageView");
    const VkResult result = next(dev, info, alloc, out);
    if (result == VK_SUCCESS && out != nullptr) {
        std::lock_guard<std::mutex> held(g_views_lock);
        g_views.insert(*out);
        g_destroyed_views.erase(*out);
        g_views_tracked.store(true, std::memory_order_relaxed);
    }
    return result;
}

extern "C" void vkDestroyImageView(VkDevice dev, VkImageView view,
                                   VkAllocationCallbacks const* alloc) {
    using Fn = void (*)(VkDevice, VkImageView, VkAllocationCallbacks const*);
    static const Fn next = real<Fn>("vkDestroyImageView");
    bool seen = true;
    {
        std::lock_guard<std::mutex> held(g_views_lock);
        seen = g_views.erase(view) != 0 || view == VK_NULL_HANDLE;
        if (g_destroyed_views.size() >= (1u << 18)) g_destroyed_views.clear();
        g_destroyed_views.insert(view);
    }
    // A view made where bg3le did not see it; the first few, then a count.
    if (!seen && g_views_tracked.load(std::memory_order_relaxed)) {
        const unsigned n = ++g_unseen_destroys;
        if (n <= 5 || (n & (n - 1)) == 0) {
            bg3le::logf("vulkan: destroying image view %p, whose creation was not seen (%u so far)",
                        (void*)view, n);
        }
    }
    next(dev, view, alloc);
}

namespace bg3le {

// Whether `view` is a live image view. True for anything when no creation
// has been seen, so a missed path cannot blank every texture.
bool vk_image_view_live(VkImageView view) {
    if (!g_views_tracked.load(std::memory_order_relaxed)) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            logf("vulkan: no image view creation seen; textures are not checked");
        }
        return true;
    }
    std::lock_guard<std::mutex> held(g_views_lock);
    return g_views.count(view) != 0;
}

std::size_t vk_image_views_tracked() {
    std::lock_guard<std::mutex> held(g_views_lock);
    return g_views.size();
}

// What the tracker knows of a view: "live", "destroyed", or "never created"
// (made where bg3le does not see it, or not a view at all).
char const* vk_image_view_state(VkImageView view) {
    if (!g_views_tracked.load(std::memory_order_relaxed)) return "untracked";
    std::lock_guard<std::mutex> held(g_views_lock);
    if (g_views.count(view) != 0) return "live";
    return g_destroyed_views.count(view) != 0 ? "destroyed" : "never created";
}

}  // namespace bg3le

BG3LE_FORWARD(void, vkDestroySwapchainKHR,
              (VkDevice dev, VkSwapchainKHR chain,
               VkAllocationCallbacks const* alloc),
              (dev, chain, alloc))

namespace {

// A present that is not VK_SUCCESS is what tells the game to rebuild its
// swapchain. The first few of each kind, then each doubling.
void note_present(VkResult result) {
    static std::atomic<unsigned> suboptimal{0}, out_of_date{0}, other{0};
    std::atomic<unsigned>& count = result == VK_SUBOPTIMAL_KHR ? suboptimal
        : result == VK_ERROR_OUT_OF_DATE_KHR ? out_of_date : other;
    const unsigned n = ++count;
    if (n <= 5 || (n & (n - 1)) == 0) {
        bg3le::logf("vulkan: present returned %d (%u so far) on %s", (int)result, n,
                    bg3le::thread_tag());
    }
}

}  // namespace

extern "C" VkResult vkQueuePresentKHR(VkQueue queue, VkPresentInfoKHR const* info) {
    using Fn = VkResult (*)(VkQueue, VkPresentInfoKHR const*);
    static const Fn next = real<Fn>("vkQueuePresentKHR");
    const Fn take = hooked<Fn>(next);
    const VkResult result = (take != nullptr && take != (Fn)&vkQueuePresentKHR)
        ? take(queue, info) : next(queue, info);
    if (result != VK_SUCCESS) note_present(result);
    return result;
}

#undef BG3LE_FORWARD

namespace {

struct Entry {
    char const* Name;
    void* Fn;
};

Entry const kEntries[] = {
    {"vkCreateInstance", (void*)&vkCreateInstance},
    {"vkCreateDevice", (void*)&vkCreateDevice},
    {"vkDestroyDevice", (void*)&vkDestroyDevice},
    {"vkCreatePipelineCache", (void*)&vkCreatePipelineCache},
    {"vkCreateSwapchainKHR", (void*)&vkCreateSwapchainKHR},
    {"vkDestroySwapchainKHR", (void*)&vkDestroySwapchainKHR},
    {"vkQueuePresentKHR", (void*)&vkQueuePresentKHR},
    {"vkCreateImageView", (void*)&vkCreateImageView},
    {"vkDestroyImageView", (void*)&vkDestroyImageView},
};

}  // namespace

namespace bg3le {

// Every pair at once, before any hook is registered.
//
// Eagerly rather than as each forwarder is first called, because bg3se wraps
// the later entry points from inside the earlier ones' hooks: vkCreateDevice
// is wrapped during vkCreateInstance, and its forwarder has not run yet, so
// waiting for it to resolve its own function would be too late.
void vulkan_register_forwarders() {
    static bool done = false;
    if (done || !imgui_overlay_wanted()) return;
    done = true;

    for (Entry const& e : kEntries) {
        void* real_one = ::dlsym(RTLD_NEXT, e.Name);
        if (real_one == nullptr && loader() != nullptr) {
            real_one = ::dlsym(loader(), e.Name);
        }
        if (real_one == nullptr || real_one == e.Fn) continue;
        detour_register_forwarder(e.Fn, real_one);
    }
    logf("imgui: registered %zu Vulkan forwarders",
         sizeof(kEntries) / sizeof(kEntries[0]));
}

}  // namespace bg3le

// Whether a proc-address lookup should be answered with a forwarder.
//
// Called by src/vulkan_memory.cpp, which owns the two proc-address hooks.
// A C symbol, and weakly referenced there, because that file is also built
// into memsteer.so -- which has no ImGui overlay and must not drag the
// forwarders in.
extern "C" void* bg3le_vulkan_forwarder(char const* name) {
    if (name == nullptr || !bg3le::imgui_overlay_wanted()) return nullptr;

    bg3le::vulkan_register_forwarders();
    for (Entry const& e : kEntries) {
        if (std::strcmp(name, e.Name) == 0) return e.Fn;
    }
    return nullptr;
}
