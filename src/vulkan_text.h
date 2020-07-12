#pragma once

#include <km_common/km_array.h>
#include <km_common/km_defines.h>
#include <km_common/km_math.h>
#include <km_common/km_string.h>

#include "load_font.h"
#include "vulkan_core.h"

//
// Shared utility for rendering text in Vulkan
//
// Your app's Vulkan state should have a VulkanTextPipeline object
// Loaded by calling LoadTextPipelineWindow(...) and LoadTextPipelineSwapchain(...), in that order
// Unloaded by calling UnloadTextPipelineSwapchain(...) and UnloadTextPipelineWindow(...), in that order
//
// Your app should have a VulkanTextRenderState, initially cleared through ResetTextRenderState(...)
// Then you can repeatedly call PushText(...) to push text render operations into the VulkanTextRenderState object
//
// When your app is filling the command buffer for rendering it, should call UploadAndSubmitTextDrawCommands(...)
// to draw all text recorded through PushText(...)
//

// TODO this should be per-app... oh well
enum class FontId
{
    OCR_A_REGULAR_18,
    OCR_A_REGULAR_24,

    COUNT
};

struct FontFace
{
    static const uint32 MAX_GLYPHS = 256;

	uint32 height;
	FixedArray<GlyphInfo, MAX_GLYPHS> glyphInfo;
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

struct VulkanTextInstanceData
{
    Vec3 pos;
    Vec2 size;
    Vec4 uvInfo;
};

struct VulkanTextRenderState
{
    using TextInstanceData = FixedArray<VulkanTextInstanceData, VulkanTextPipeline::MAX_INSTANCES>;
    StaticArray<TextInstanceData, VulkanTextPipeline::MAX_FONTS> textInstanceData;
};

void PushText(FontId fontId, const_string text, Vec2Int pos, float32 depth, Vec2Int screenSize,
              const VulkanTextPipeline& textPipeline, VulkanTextRenderState* renderState);

void ResetTextRenderState(VulkanTextRenderState* renderState);

void UploadAndSubmitTextDrawCommands(VkDevice device, VkCommandBuffer commandBuffer,
                                     const VulkanTextPipeline& textPipeline, const VulkanTextRenderState& renderState,
                                     LinearAllocator* allocator);

bool LoadTextPipelineSwapchain(const VulkanWindow& window, const VulkanSwapchain& swapchain, LinearAllocator* allocator,
                               VulkanTextPipeline* textPipeline);
void UnloadTextPipelineSwapchain(VkDevice device, VulkanTextPipeline* textPipeline);

bool LoadTextPipelineWindow(const VulkanWindow& window, VkCommandPool commandPool, LinearAllocator* allocator,
                            VulkanTextPipeline* textPipeline);
void UnloadTextPipelineWindow(VkDevice device, VulkanTextPipeline* textPipeline);
