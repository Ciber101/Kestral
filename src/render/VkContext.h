#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <string>
#include <vector>

struct GLFWwindow;

namespace kst {

// Instance, device, queue, allocator and swapchain.
struct VkContext {
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator vma = nullptr;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> renderDone;   // one per swapchain image
    uint32_t minImageCount = 2;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

    std::string gpuName;
    bool wideLines = false;
    bool multiDrawIndirect = false;
    float lineWidthMax = 1.f;
    VkSampleCountFlagBits msaa = VK_SAMPLE_COUNT_4_BIT;
    bool validation = false;

    bool init(GLFWwindow* window, bool enableValidation, std::string& err);
    bool createSwapchain(std::string& err);   // creates or recreates
    void destroySwapchain();
    void shutdown();
};

const char* vkResultName(VkResult r);

} // namespace kst
