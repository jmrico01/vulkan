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

#include "mesh.h"

const uint32 BLOCKS_SIZE_X = 64;
const uint32 BLOCKS_SIZE_Y = 64;
const uint32 BLOCKS_SIZE_Z = 16;

const uint32 BLOCK_ORIGIN_X = BLOCKS_SIZE_X / 2;
const uint32 BLOCK_ORIGIN_Y = BLOCKS_SIZE_Y / 2;
const uint32 BLOCK_ORIGIN_Z = 1;

enum class BlockId
{
    NONE = 0,
    BLOCK,
};

enum class SpriteId
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

struct VulkanAppState
{
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffer;
    VkFence fence;

    VulkanSpritePipeline<(uint32)SpriteId::COUNT> spritePipeline;
    VulkanTextPipeline<(uint32)FontId::COUNT> textPipeline;

    VulkanMeshPipeline meshPipeline;
    VulkanLightmapMeshPipeline lightmapMeshPipeline;
};

struct AppState
{
    VulkanAppState vulkanAppState;
    FontFace fontFaces[FontId::COUNT];

    BlockId blocks[BLOCKS_SIZE_Z][BLOCKS_SIZE_Y][BLOCKS_SIZE_X];
    float32 totalElapsed;
    Vec3 cameraPos;
    Vec2 cameraAngles;
};

struct FrameState
{
    VulkanSpriteRenderState<(uint32)SpriteId::COUNT> spriteRenderState;
    VulkanTextRenderState<(uint32)FontId::COUNT> textRenderState;

    VulkanMeshRenderState meshRenderState;
};

struct TransientState
{
    FrameState frameState;
    LargeArray<uint8> scratch;
};
