#pragma once

// NOTE this header is a good candidate for putting in km_common or a new km_app lib or something

#include <km_common/km_input.h>
#include <km_common/km_math.h>

#include "vulkan.h"

struct AppMemory
{
    bool initialized;
    LargeArray<uint8> permanent;
    LargeArray<uint8> transient;
};

#define APP_WORK_QUEUE_CALLBACK_FUNCTION(name) void name(void* data);
typedef APP_WORK_QUEUE_CALLBACK_FUNCTION(AppWorkQueueCallbackFunction);

struct AppWorkEntry
{
    AppWorkQueueCallbackFunction* callback;
    void* data;
};

struct AppWorkQueue
{
    volatile uint32 entriesTotal;
    volatile uint32 entriesComplete;

    volatile uint32 read;
    volatile uint32 write;
    AppWorkEntry entries[4 * 1024];
    HANDLE win32SemaphoreHandle;
};

void CompleteAllWork(AppWorkQueue* queue);
bool TryAddWork(AppWorkQueue* queue, AppWorkQueueCallbackFunction* callback, void* data);

struct AppAudio
{
};

#define APP_UPDATE_AND_RENDER_FUNCTION(name) bool name(const VulkanState& vulkanState, uint32_t swapchainImageIndex, \
const AppInput& input, float32 deltaTime, \
AppMemory* memory, AppAudio* audio, AppWorkQueue* queue)
typedef APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRenderFunction);

APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender);

#define APP_LOAD_VULKAN_STATE_FUNCTION(name) bool name(const VulkanState& vulkanState, AppMemory* memory)
typedef APP_LOAD_VULKAN_STATE_FUNCTION(AppLoadVulkanStateFunction);
APP_LOAD_VULKAN_STATE_FUNCTION(AppLoadVulkanState);

#define APP_UNLOAD_VULKAN_STATE_FUNCTION(name) void name(const VulkanState& vulkanState, AppMemory* memory)
typedef APP_UNLOAD_VULKAN_STATE_FUNCTION(AppUnloadVulkanStateFunction);
APP_UNLOAD_VULKAN_STATE_FUNCTION(AppUnloadVulkanState);
