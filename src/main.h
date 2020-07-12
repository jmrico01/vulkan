#pragma once

#include <stdio.h>

#define LOG_ERROR(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_FLUSH() fflush(stderr); fflush(stdout)

#include <km_common/km_array.h>
#include <km_common/km_debug.h>

#include "load_font.h"
#include "vulkan_core.h"

enum class FontId
{
    OCR_A_REGULAR_18,
    OCR_A_REGULAR_24,

    COUNT
};

enum class SpriteId
{
    JON,
    ROCK,

    COUNT
};

struct FontFace
{
    static const uint32 MAX_GLYPHS = 256;

	uint32 height;
	FixedArray<GlyphInfo, MAX_GLYPHS> glyphInfo;
};

// TODO move this to vulkan libs?
struct VulkanImage
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
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

struct VulkanTextPipeline
{
    static const uint32 MAX_FONTS = (uint32)FontId::COUNT;
    static const uint32 MAX_INSTANCES = 4096;

    FontFace fontFaces[MAX_FONTS];

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;

    VkBuffer instanceBuffer;
    VkDeviceMemory instanceBufferMemory;

    VulkanImage atlases[MAX_FONTS];
    VkSampler atlasSampler;

    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSets[MAX_FONTS];

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

struct VulkanTextInstanceData
{
    Vec3 pos;
    Vec2 size;
    Vec4 uvInfo;
};

struct FrameState
{
    using SpriteInstanceData = FixedArray<VulkanSpriteInstanceData, VulkanSpritePipeline::MAX_INSTANCES>;
    StaticArray<SpriteInstanceData, VulkanSpritePipeline::MAX_SPRITES> spriteInstanceData;

    using TextInstanceData = FixedArray<VulkanTextInstanceData, VulkanTextPipeline::MAX_INSTANCES>;
    StaticArray<TextInstanceData, VulkanTextPipeline::MAX_FONTS> textInstanceData;
};

struct TransientState
{
    FrameState frameState;
    LargeArray<uint8> scratch;
};
