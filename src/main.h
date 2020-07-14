#pragma once

#include <stdio.h>

#define LOG_ERROR(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_FLUSH() fflush(stderr); fflush(stdout)

#include <km_common/km_array.h>
#include <km_common/km_debug.h>
#include <km_common/vulkan/km_vulkan_core.h>
#include <km_common/vulkan/km_vulkan_sprite.h>
#include <km_common/vulkan/km_vulkan_text.h>

enum SpriteId
{
    JON,
    ROCK,

    COUNT
};

enum class FontId
{
    OCR_A_REGULAR_18,
    OCR_A_REGULAR_24,

    COUNT
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

    VulkanSpritePipeline<(uint32)SpriteId::COUNT> spritePipeline;
    VulkanTextPipeline<(uint32)FontId::COUNT> textPipeline;
    VulkanMeshPipeline meshPipeline;
};

struct AppState
{
    VulkanAppState vulkanAppState;
    FontFace fontFaces[FontId::COUNT];

    float32 totalElapsed;
    Vec3 cameraPos;
    Vec2 cameraAngles;
};

struct FrameState
{
    VulkanSpriteRenderState<(uint32)SpriteId::COUNT> spriteRenderState;
    VulkanTextRenderState<(uint32)FontId::COUNT> textRenderState;
};

struct TransientState
{
    FrameState frameState;
    LargeArray<uint8> scratch;
};
