#define VMA_IMPLEMENTATION
#include "render/VkContext.h"
#include "core/Log.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>

namespace kst {

const char* vkResultName(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    default: return "VkResult error";
    }
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        log::warn(std::string("[vulkan] ") + (data && data->pMessage ? data->pMessage : ""));
    return VK_FALSE;
}

bool VkContext::init(GLFWwindow* win, bool enableValidation, std::string& err) {
    window = win;
    uint32_t apiVersion = 0;
    vkEnumerateInstanceVersion(&apiVersion);
    if (apiVersion < VK_API_VERSION_1_3) {
        err = "Vulkan 1.3 is required. Please update your graphics driver.";
        return false;
    }

    uint32_t extCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&extCount);
    std::vector<const char*> exts(glfwExts, glfwExts + extCount);
    std::vector<const char*> layers;
    if (enableValidation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props)
            if (std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                validation = true;
            }
        if (!validation) log::warn("Validation requested but VK_LAYER_KHRONOS_validation not installed");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Kestral";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Kestral";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.data();
    VkResult res = vkCreateInstance(&ici, nullptr, &instance);
    if (res != VK_SUCCESS) { err = std::string("vkCreateInstance failed: ") + vkResultName(res); return false; }

    if (validation) {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (fn) {
            VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugCallback;
            fn(instance, &dci, nullptr, &messenger);
        }
    }

    if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
        err = "Could not create a Vulkan surface for the window.";
        return false;
    }

    // Pick the best GPU: discrete first, must support 1.3, swapchain, present, geometry shader stage features.
    uint32_t pc = 0;
    vkEnumeratePhysicalDevices(instance, &pc, nullptr);
    std::vector<VkPhysicalDevice> devs(pc);
    vkEnumeratePhysicalDevices(instance, &pc, devs.data());
    int bestScore = -1;
    for (auto d : devs) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(d, &f2);
        if (!f13.dynamicRendering || !f13.synchronization2 || !f2.features.geometryShader) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
        int fam = -1;
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
            if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { fam = (int)i; break; }
        }
        if (fam < 0) continue;
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 :
                    props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 100 : 10;
        if (score > bestScore) {
            bestScore = score;
            phys = d;
            queueFamily = (uint32_t)fam;
            gpuName = props.deviceName;
            wideLines = f2.features.wideLines;
            multiDrawIndirect = f2.features.multiDrawIndirect;
            lineWidthMax = props.limits.lineWidthRange[1];
            VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
            // 8x on desktop GPUs: crisp silhouettes and edges for almost no cost on modern cards.
            msaa = (counts & VK_SAMPLE_COUNT_8_BIT) ? VK_SAMPLE_COUNT_8_BIT :
                   (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT :
                   (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
        }
    }
    if (!phys) {
        err = "No GPU with Vulkan 1.3 dynamic rendering found. Please update your graphics driver.";
        return false;
    }
    log::info("GPU: " + gpuName);

    float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.geometryShader = VK_TRUE;
    f2.features.wideLines = wideLines ? VK_TRUE : VK_FALSE;
    f2.features.multiDrawIndirect = multiDrawIndirect ? VK_TRUE : VK_FALSE;
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    res = vkCreateDevice(phys, &dci, nullptr, &device);
    if (res != VK_SUCCESS) { err = std::string("vkCreateDevice failed: ") + vkResultName(res); return false; }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VmaAllocatorCreateInfo aci{};
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.physicalDevice = phys;
    aci.device = device;
    aci.instance = instance;
    if (vmaCreateAllocator(&aci, &vma) != VK_SUCCESS) { err = "Could not create GPU memory allocator"; return false; }

    return createSwapchain(err);
}

bool VkContext::createSwapchain(std::string& err) {
    vkDeviceWaitIdle(device);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) ext = {(uint32_t)fbw, (uint32_t)fbh};
    ext.width = std::clamp(ext.width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
    ext.height = std::clamp(ext.height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));
    if (ext.width == 0 || ext.height == 0) return false;   // minimized

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fn, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (auto& f : fmts)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }

    presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync: smooth, no tearing, no wasted power

    uint32_t count = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);
    minImageCount = std::max(caps.minImageCount, 2u);

    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    VkResult res = vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
    if (old) {
        for (auto v : views) vkDestroyImageView(device, v, nullptr);
        for (auto s : renderDone) vkDestroySemaphore(device, s, nullptr);
        views.clear();
        renderDone.clear();
        vkDestroySwapchainKHR(device, old, nullptr);
    }
    if (res != VK_SUCCESS) { swapchain = VK_NULL_HANDLE; err = std::string("vkCreateSwapchainKHR failed: ") + vkResultName(res); return false; }

    swapFormat = chosen.format;
    colorSpace = chosen.colorSpace;
    extent = ext;
    uint32_t ic = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &ic, nullptr);
    images.resize(ic);
    vkGetSwapchainImagesKHR(device, swapchain, &ic, images.data());
    views.resize(ic);
    renderDone.resize(ic);
    for (uint32_t i = 0; i < ic; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &vci, nullptr, &views[i]);
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &sem, nullptr, &renderDone[i]);
    }
    return true;
}

void VkContext::destroySwapchain() {
    if (!device) return;
    for (auto v : views) vkDestroyImageView(device, v, nullptr);
    for (auto s : renderDone) vkDestroySemaphore(device, s, nullptr);
    views.clear();
    renderDone.clear();
    images.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void VkContext::shutdown() {
    if (device) vkDeviceWaitIdle(device);
    destroySwapchain();
    if (vma) vmaDestroyAllocator(vma);
    vma = nullptr;
    if (device) vkDestroyDevice(device, nullptr);
    device = VK_NULL_HANDLE;
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    if (messenger) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(instance, messenger, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE;
}

} // namespace kst
