#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <km_common/km_array.h>
#include <km_common/km_math.h>
#include <km_common/km_memory.h>

struct VulkanState
{
    static const uint32 MAX_SWAPCHAIN_IMAGES = 16;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkSurfaceKHR surface;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;

    VkSwapchainKHR swapchain;
    VkFormat swapchainImageFormat;
    VkExtent2D swapchainExtent;

    FixedArray<VkImage, MAX_SWAPCHAIN_IMAGES> swapchainImages;
    FixedArray<VkImageView, MAX_SWAPCHAIN_IMAGES> swapchainImageViews;

    VkRenderPass renderPass;
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;
    FixedArray<VkFramebuffer, MAX_SWAPCHAIN_IMAGES> swapchainFramebuffers;

    VkCommandPool commandPool;
    FixedArray<VkCommandBuffer, MAX_SWAPCHAIN_IMAGES> commandBuffers;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;

    VkImage textureImage;
    VkDeviceMemory textureImageMemory;
    VkImageView textureImageView;
    VkSampler textureSampler;

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    VkBuffer indexBuffer;
    VkDeviceMemory indexBufferMemory;
    VkBuffer uniformBuffer;
    VkDeviceMemory uniformBufferMemory;

    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSet;

    VkSemaphore imageAvailableSemaphore;
    VkSemaphore renderFinishedSemaphore;
};

bool RecreateVulkanSwapchain(VulkanState* state, Vec2Int size, LinearAllocator* allocator);
bool LoadVulkanState(VulkanState* state, HINSTANCE hInstance, HWND hWnd, Vec2Int size, LinearAllocator* allocator);

void UnloadVulkanSwapchain(VulkanState* state);
void UnloadVulkanState(VulkanState* state);
