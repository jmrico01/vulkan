#pragma once

#include <km_common/app/km_log.h>

#include <km_common/km_array.h>
#include <km_common/km_debug.h>
#include <km_common/vulkan/km_vulkan_core.h>
#include <km_common/vulkan/km_vulkan_sprite.h>
#include <km_common/vulkan/km_vulkan_text.h>

#include "imgui.h"
#include "mesh.h"

constexpr Vec3Int BLOCKS_SIZE = { 64, 64, 16 };
constexpr Vec3Int BLOCK_ORIGIN = { BLOCKS_SIZE.x / 2, BLOCKS_SIZE.y / 2, 1 };

enum class BlockId
{
    NONE = 0,
    SIDEWALK,
    STREET,
    BUILDING,

    COUNT
};

struct Block
{
    BlockId id;
};

using BlockGrid = StaticArray<StaticArray<StaticArray<Block, BLOCKS_SIZE.x>, BLOCKS_SIZE.y>, BLOCKS_SIZE.z>;

enum class SpriteId
{
    PIXEL,
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
    VulkanFontFace fontFaces[FontId::COUNT];

    float32 elapsedTime;

    BlockGrid blockGrid;
    float32 blockSize;

    Vec3 cameraPos;
    Vec2 cameraAngles;

    // Debug
    bool debugView;

    bool noclip;
    Vec3 noclipPos;

    bool cityGenMinimized;
    PanelInputIntState inputStreetSize;
    PanelInputIntState inputSidewalkSize;
    PanelInputIntState inputBuildingSize;
    PanelInputIntState inputBuildingHeight;
    PanelSliderState sliderBlockSize;

    bool blockEditor;
    uint32 selectedZ;
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
