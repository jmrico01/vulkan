#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <km_common/km_array.h>
#include <km_common/km_math.h>
#include <km_common/km_memory.h>

struct QueueFamilyInfo
{
    bool hasGraphicsFamily;
    uint32_t graphicsFamilyIndex;
    bool hasPresentFamily;
    uint32_t presentFamilyIndex;
};

QueueFamilyInfo GetQueueFamilyInfo(VkSurfaceKHR surface, VkPhysicalDevice physicalDevice, LinearAllocator* allocator);

bool CreateShaderModule(const Array<uint8> code, VkDevice device, VkShaderModule* shaderModule);

bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryPropertyFlags,
                  VkDevice device, VkPhysicalDevice physicalDevice, VkBuffer* buffer, VkDeviceMemory* bufferMemory);

void CopyBuffer(VkDevice device, VkCommandPool commandPool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size);

bool CreateImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height, VkFormat format,
                 VkImageTiling tiling, VkImageUsageFlags usageFlags, VkMemoryPropertyFlags memoryPropertyFlags,
                 VkImage* image, VkDeviceMemory* imageMemory);

void TransitionImageLayout(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkImage image, VkFormat format,
                           VkImageLayout oldLayout, VkImageLayout newLayout);

void CopyBufferToImage(VkDevice device, VkCommandPool commandPool, VkQueue queue,
                       VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

bool CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectFlags,
                     VkImageView* imageView);

// Core Vulkan state, Should be initialized once in the entire application
struct VulkanCore
{
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
};

// Window/surface-dependent Vulkan state, needs to be recreated when window properties change (e.g. fullscreen)
struct VulkanWindow
{
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;

    VkSemaphore imageAvailableSemaphore;
    VkSemaphore renderFinishedSemaphore;
};

// Swapchain-dependent Vulkan state, needs to be recreated when application render targets change (e.g. window size)
struct VulkanSwapchain
{
    static const uint32 MAX_IMAGES = 16;

    VkSwapchainKHR swapchain;
    VkFormat imageFormat;
    VkExtent2D extent;

    FixedArray<VkImage, MAX_IMAGES> images;
    FixedArray<VkImageView, MAX_IMAGES> imageViews;
    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;

    VkRenderPass renderPass;
    FixedArray<VkFramebuffer, MAX_IMAGES> framebuffers;
};

struct VulkanState
{
    VulkanCore core;
    VulkanWindow window;
    VulkanSwapchain swapchain;
};

// Reloads (explicit unload & load) VulkanSwapchain and all dependent state
bool ReloadVulkanSwapchain(VulkanState* state, Vec2Int size, LinearAllocator* allocator);

// Reloads (explicit unload & load) VulkanWindow and all dependent state
bool ReloadVulkanWindow(VulkanState* state, HINSTANCE hInstance, HWND hWnd, Vec2Int size, LinearAllocator* allocator);

bool LoadVulkanState(VulkanState* state, HINSTANCE hInstance, HWND hWnd, Vec2Int size, LinearAllocator* allocator);
void UnloadVulkanState(VulkanState* state);
