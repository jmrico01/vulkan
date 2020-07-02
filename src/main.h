#pragma once

#include <stdio.h>

#define LOG_ERROR(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_FLUSH() fflush(stderr); fflush(stdout)

#include <km_common/km_array.h>
#include <km_common/km_debug.h>

#include "vulkan.h"

struct VulkanImage
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

struct VulkanAppState
{
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;

    VkCommandPool commandPool;
    FixedArray<VkCommandBuffer, VulkanSwapchain::MAX_IMAGES> commandBuffers;

    static const uint64 MAX_LIGHTMAPS = 64;
    FixedArray<VulkanImage, MAX_LIGHTMAPS> lightmaps;
    VkSampler textureSampler;

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    VkBuffer uniformBuffer;
    VkDeviceMemory uniformBufferMemory;

    VkDescriptorPool descriptorPool;
    FixedArray<VkDescriptorSet, MAX_LIGHTMAPS> descriptorSets;
};

struct AppState
{
    VulkanAppState vulkanAppState;

    float32 totalElapsed;
    Vec3 cameraPos;
    Vec2 cameraAngles;
};