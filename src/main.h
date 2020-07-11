#pragma once

#include <stdio.h>

#define LOG_ERROR(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_FLUSH() fflush(stderr); fflush(stdout)

#include <km_common/km_array.h>
#include <km_common/km_debug.h>

#include "vulkan.h"

// TODO move this to vulkan libs?
struct VulkanImage
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

struct VulkanSpritePipeline
{
    static const uint32 MAX_SPRITES = 32;
    FixedArray<VulkanImage, MAX_SPRITES> sprites;
    VkSampler spriteSampler;

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;

    VkBuffer uniformBuffer;
    VkDeviceMemory uniformBufferMemory;

    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    FixedArray<VkDescriptorSet, MAX_SPRITES> descriptorSets;

    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
};

struct VulkanMeshPipeline
{
    static const uint32 MAX_MESHES = 64;
    FixedArray<uint32, MAX_MESHES> meshTriangleEndInds;
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;

    static const uint32 MAX_LIGHTMAPS = 64;
    FixedArray<VulkanImage, MAX_LIGHTMAPS> lightmaps;
    VkSampler lightmapSampler;

    VkBuffer uniformBuffer;
    VkDeviceMemory uniformBufferMemory;

    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    FixedArray<VkDescriptorSet, MAX_LIGHTMAPS> descriptorSets;

    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
};

struct VulkanAppState
{
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffer;
    VkFence fence;

    VulkanSpritePipeline spritePipeline;
    VulkanMeshPipeline meshPipeline;
};

struct AppState
{
    VulkanAppState vulkanAppState;

    float32 totalElapsed;
    Vec3 cameraPos;
    Vec2 cameraAngles;
};