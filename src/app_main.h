#pragma once

// NOTE this header is a good candidate for putting in km_common or a new km_app lib or something

#include <km_common/km_input.h>
#include <km_common/km_math.h>

#include "vulkan.h"

struct AppMemory
{
    bool initialized;
    Array<uint8> permanent;
    Array<uint8> transient;
};

struct AppAudio
{
};

#define APP_UPDATE_AND_RENDER_FUNCTION(name) bool name(const VulkanState& vulkanState, uint32_t swapchainImageIndex, \
const AppInput& input, float32 deltaTime, \
AppMemory* memory, AppAudio* audio)
typedef APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRenderFunction);

APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender);

#define APP_LOAD_VULKAN_STATE_FUNCTION(name) bool name(const VulkanState& vulkanState, AppMemory* memory)
typedef APP_LOAD_VULKAN_STATE_FUNCTION(AppLoadVulkanStateFunction);
APP_LOAD_VULKAN_STATE_FUNCTION(AppLoadVulkanState);

#define APP_UNLOAD_VULKAN_STATE_FUNCTION(name) void name(const VulkanState& vulkanState, AppMemory* memory)
typedef APP_UNLOAD_VULKAN_STATE_FUNCTION(AppUnloadVulkanStateFunction);
APP_UNLOAD_VULKAN_STATE_FUNCTION(AppUnloadVulkanState);
