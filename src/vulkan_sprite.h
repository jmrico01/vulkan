#pragma once

#include <km_common/km_defines.h>
#include <km_common/km_math.h>

#include "vulkan_core.h"

//
// Shared utility for rendering sprites in Vulkan
//
// Your app's Vulkan state should have a VulkanSpritePipeline object
// Loaded by calling LoadSpritePipelineWindow(...) and LoadSpritePipelineSwapchain(...), in that order
// Unloaded by calling UnloadSpritePipelineSwapchain(...) and UnloadSpritePipelineWindow(...), in that order
//
// Your app should have a VulkanSpriteRenderState, initially cleared through ResetSpriteRenderState(...)
// Then you can repeatedly call PushSprite(...) to push sprite render operations into the VulkanSpriteRenderState object
//
// When your app is filling the command buffer for rendering, you should call UploadAndSubmitTextDrawCommands(...)
// to draw all sprites recorded through PushSprite(...)
//

// TODO this should be per-app... oh well
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

struct VulkanSpriteVertex
{
    Vec2 pos;
    Vec2 uv;
};

struct VulkanSpriteInstanceData
{
    Vec3 pos;
    Vec2 size;
};

struct VulkanSpriteRenderState
{
    using SpriteInstanceData = FixedArray<VulkanSpriteInstanceData, VulkanSpritePipeline::MAX_INSTANCES>;
    StaticArray<SpriteInstanceData, VulkanSpritePipeline::MAX_SPRITES> spriteInstanceData;
};

void PushSprite(SpriteId spriteId, Vec2Int pos, Vec2Int size, float32 depth, Vec2Int screenSize,
                VulkanSpriteRenderState* renderState);

void ResetSpriteRenderState(VulkanSpriteRenderState* renderState);

void UploadAndSubmitSpriteDrawCommands(VkDevice device, VkCommandBuffer commandBuffer,
                                       const VulkanSpritePipeline& spritePipeline, const VulkanSpriteRenderState& renderState,
                                       LinearAllocator* allocator);

bool LoadSpritePipelineSwapchain(const VulkanWindow& window, const VulkanSwapchain& swapchain, LinearAllocator* allocator,
                                 VulkanSpritePipeline* spritePipeline);
void UnloadSpritePipelineSwapchain(VkDevice device, VulkanSpritePipeline* spritePipeline);

bool LoadSpritePipelineWindow(const VulkanWindow& window, VkCommandPool commandPool, LinearAllocator* allocator,
                              VulkanSpritePipeline* spritePipeline);
void UnloadSpritePipelineWindow(VkDevice device, VulkanSpritePipeline* spritePipeline);
