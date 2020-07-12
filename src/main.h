#pragma once

#include <stdio.h>

#define LOG_ERROR(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_FLUSH() fflush(stderr); fflush(stdout)

#include <km_common/km_array.h>
#include <km_common/km_debug.h>

#include "load_font.h"
#include "vulkan_core.h"
#include "vulkan_text.h"

enum class SpriteId
{
    JON,
    ROCK,

    COUNT
};

struct VulkanSpritePipeline
{
    static const uint32 MAX_SPRITES = (uint32)SpriteId::COUNT;
    static const uint32 MAX_INSTANCES = 64;

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;

    VkBuffer instanceBuffer;
    VkDeviceMemory instanceBufferMemory;

    VulkanImage sprites[MAX_SPRITES];
    VkSampler spriteSampler;

    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSets[MAX_SPRITES];

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
    VulkanTextPipeline textPipeline;
    VulkanMeshPipeline meshPipeline;
};

struct AppState
{
    VulkanAppState vulkanAppState;

    float32 totalElapsed;
    Vec3 cameraPos;
    Vec2 cameraAngles;
};

struct VulkanSpriteInstanceData
{
    Vec3 pos;
    Vec2 size;
};

struct FrameState
{
    using SpriteInstanceData = FixedArray<VulkanSpriteInstanceData, VulkanSpritePipeline::MAX_INSTANCES>;
    StaticArray<SpriteInstanceData, VulkanSpritePipeline::MAX_SPRITES> spriteInstanceData;

    VulkanTextRenderState textRenderState;
};

struct TransientState
{
    FrameState frameState;
    LargeArray<uint8> scratch;
};
