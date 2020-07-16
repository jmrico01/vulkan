#include "main.h"

#include <intrin.h>
#include <stdio.h>

#include <stb_image.h>

#include <km_common/km_array.h>
#include <km_common/km_defines.h>
#include <km_common/km_load_obj.h>
#include <km_common/km_os.h>
#include <km_common/km_string.h>
#include <km_common/app/km_app.h>

#include "lightmap.h"

#define ENABLE_THREADS 1
#define ENABLE_LIGHTMAPPED_MESH 1

// Required for platform main
const char* WINDOW_NAME = "vulkan";
const int WINDOW_START_WIDTH  = 1600;
const int WINDOW_START_HEIGHT = 900;
const uint64 PERMANENT_MEMORY_SIZE = MEGABYTES(1);
const uint64 TRANSIENT_MEMORY_SIZE = MEGABYTES(256);

internal AppState* GetAppState(AppMemory* memory)
{
    DEBUG_ASSERT(sizeof(AppState) < memory->permanent.size);
    return (AppState*)memory->permanent.data;
}

internal TransientState* GetTransientState(AppMemory* memory)
{
    DEBUG_ASSERT(sizeof(TransientState) < memory->transient.size);
    TransientState* transientState = (TransientState*)memory->transient.data;
    transientState->scratch = {
        .size = memory->transient.size - sizeof(TransientState),
        .data = memory->transient.data + sizeof(TransientState),
    };
    return transientState;
}

APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender)
{
    UNREFERENCED_PARAMETER(audio);

    AppState* appState = GetAppState(memory);
    TransientState* transientState = GetTransientState(memory);

    const Vec2Int screenSize = {
        (int)vulkanState.swapchain.extent.width,
        (int)vulkanState.swapchain.extent.height
    };

    // Initialize memory if necessary
    if (!memory->initialized) {
        appState->totalElapsed = 0.0f;
        appState->cameraPos = Vec3 { -1.0f, 0.0f, 1.0f };
        appState->cameraAngles = Vec2 { 0.0f, 0.0f };

        // TODO this should run on window recreation, fullscreen wipes all sprites
        // Sprites
        {
            const char* spriteFilePaths[] = {
                "data/sprites/jon.png",
                "data/sprites/rock.png"
            };

            for (uint32 i = 0; i < C_ARRAY_LENGTH(spriteFilePaths); i++) {
                int width, height, channels;
                unsigned char* imageData = stbi_load(spriteFilePaths[i], &width, &height, &channels, 0);
                if (imageData == NULL) {
                    DEBUG_PANIC("Failed to load sprite: %s\n", spriteFilePaths[i]);
                }
                defer(stbi_image_free(imageData));

                VulkanImage sprite;
                if (!LoadVulkanImage(vulkanState.window.device, vulkanState.window.physicalDevice,
                                     vulkanState.window.graphicsQueue, appState->vulkanAppState.commandPool,
                                     width, height, channels, (const uint8*)imageData, &sprite)) {
                    DEBUG_PANIC("Failed to Vulkan image for sprite %s\n", spriteFilePaths[i]);
                }

                uint32 spriteIndex;
                if (!RegisterSprite(vulkanState.window.device, &appState->vulkanAppState.spritePipeline, sprite,
                                    &spriteIndex)) {
                    DEBUG_PANIC("Failed to register sprite %s\n", spriteFilePaths[i]);
                }
                DEBUG_ASSERT(spriteIndex == i);
            }
        }

        // TODO this should run on window recreation, fullscreen wipes all fonts
        // Fonts
        {
            LinearAllocator allocator(transientState->scratch);

            struct FontData {
                const_string filePath;
                uint32 height;
            };
            const FontData fontData[] = {
                { ToString("data/fonts/ocr-a/regular.ttf"), 18 },
                { ToString("data/fonts/ocr-a/regular.ttf"), 24 },
            };

            FT_Library ftLibrary;
            FT_Error error = FT_Init_FreeType(&ftLibrary);
            if (error) {
                DEBUG_PANIC("FreeType init error: %d\n", error);
            }

            for (uint32 i = 0; i < C_ARRAY_LENGTH(fontData); i++) {
                LoadFontFaceResult fontFace;
                if (!LoadFontFace(ftLibrary, fontData[i].filePath, fontData[i].height, &allocator, &fontFace)) {
                    DEBUG_PANIC("Failed to load font face at %.*s\n", fontData[i].filePath.size, fontData[i].filePath.data);
                }

                appState->fontFaces[i].height = fontFace.height;
                appState->fontFaces[i].glyphInfo.FromArray(fontFace.glyphInfo);

                VulkanImage fontAtlas;
                if (!LoadVulkanImage(vulkanState.window.device, vulkanState.window.physicalDevice,
                                     vulkanState.window.graphicsQueue, appState->vulkanAppState.commandPool,
                                     fontFace.atlasWidth, fontFace.atlasHeight, 1, fontFace.atlasData, &fontAtlas)) {
                    DEBUG_PANIC("Failed to Vulkan image for font atlas %lu\n", i);
                }

                uint32 fontIndex;
                if (!RegisterFont(vulkanState.window.device, &appState->vulkanAppState.textPipeline, fontAtlas,
                                  &fontIndex)) {
                    DEBUG_PANIC("Failed to register font %lu\n", i);
                }
                DEBUG_ASSERT(fontIndex == i);
            }
        }

        memory->initialized = true;
    }

    // Reset frame state
    {
        ResetSpriteRenderState(&transientState->frameState.spriteRenderState);
        ResetTextRenderState(&transientState->frameState.textRenderState);

        ResetMeshRenderState(&transientState->frameState.meshRenderState);
    }

#if ENABLE_LIGHTMAPPED_MESH
    if (KeyPressed(input, KM_KEY_L)) {
        LinearAllocator allocator(transientState->scratch);

        LoadObjResult obj;
        if (LoadObj(ToString("data/models/reference-scene-small.obj"), &obj, &allocator)) {
            if (GenerateLightmaps(obj, LIGHTMAP_NUM_BOUNCES, queue, &allocator, ToString("data/lightmaps"))) {
                AppUnloadVulkanSwapchainState(vulkanState, memory);
                AppUnloadVulkanWindowState(vulkanState, memory);
                if (!AppLoadVulkanWindowState(vulkanState, memory)) {
                    LOG_ERROR("Failed to reload Vulkan state after lightmap generation\n");
                }
                if (!AppLoadVulkanSwapchainState(vulkanState, memory)) {
                    LOG_ERROR("Failed to reload Vulkan state after lightmap generation\n");
                }
            }
            else {
                LOG_ERROR("Failed to generate lightmaps\n");
            }
        }
        else {
            LOG_ERROR("Failed to load scene .obj when generating lightmaps\n");
        }
    }
#endif

    appState->totalElapsed += deltaTime;

    const float32 cameraSensitivity = 2.0f;
    if (MouseDown(input, KM_MOUSE_LEFT)) {
        const Vec2 mouseDeltaFrac = {
            (float32)input.mouseDelta.x / (float32)screenSize.x,
            (float32)input.mouseDelta.y / (float32)screenSize.y
        };
        appState->cameraAngles += mouseDeltaFrac * cameraSensitivity;

        appState->cameraAngles.x = ModFloat32(appState->cameraAngles.x, PI_F * 2.0f);
        appState->cameraAngles.y = ClampFloat32(appState->cameraAngles.y, -PI_F, PI_F);
    }

    const Quat cameraRotYaw = QuatFromAngleUnitAxis(appState->cameraAngles.x, Vec3::unitZ);
    const Quat cameraRotPitch = QuatFromAngleUnitAxis(appState->cameraAngles.y, Vec3::unitY);

    const Quat cameraRotYawInv = Inverse(cameraRotYaw);
    const Vec3 cameraForward = cameraRotYawInv * Vec3::unitX;
    const Vec3 cameraRight = cameraRotYawInv * -Vec3::unitY;
    const Vec3 cameraUp = Vec3::unitZ;

    const float32 speed = 2.0f;
    if (KeyDown(input, KM_KEY_W)) {
        appState->cameraPos += speed * cameraForward * deltaTime;
    }
    if (KeyDown(input, KM_KEY_S)) {
        appState->cameraPos -= speed * cameraForward * deltaTime;
    }
    if (KeyDown(input, KM_KEY_A)) {
        appState->cameraPos -= speed * cameraRight * deltaTime;
    }
    if (KeyDown(input, KM_KEY_D)) {
        appState->cameraPos += speed * cameraRight * deltaTime;
    }
    if (KeyDown(input, KM_KEY_SPACE)) {
        appState->cameraPos += speed * cameraUp * deltaTime;
    }
    if (KeyDown(input, KM_KEY_SHIFT)) {
        appState->cameraPos -= speed * cameraUp * deltaTime;
    }

    const Quat cameraRot = cameraRotPitch * cameraRotYaw;
    const Mat4 cameraRotMat4 = UnitQuatToMat4(cameraRot);

    // Transforms world-view camera (+X forward, +Z up) to Vulkan camera (+Z forward, -Y up)
    const Quat baseCameraRot = QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitY)
        * QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitX);
    const Mat4 baseCameraRotMat4 = UnitQuatToMat4(baseCameraRot);

    const Mat4 view = baseCameraRotMat4 * cameraRotMat4 * Translate(-appState->cameraPos);

    const float32 aspect = (float32)screenSize.x / (float32)screenSize.y;
    const float32 nearZ = 0.1f;
    const float32 farZ = 50.0f;
    const Mat4 proj = Perspective(PI_F / 4.0f, aspect, nearZ, farZ);

    const uint32 NUM_JONS = 0;
    for (uint32 i = 0; i < NUM_JONS; i++) {
        const Vec2Int pos = { RandInt(0, screenSize.x), RandInt(0, screenSize.y) };
        const Vec2Int size = { RandInt(50, 300), RandInt(50, 300) };
        PushSprite(SpriteId::JON, pos, size, 0.5f, screenSize, &transientState->frameState.spriteRenderState);
    }

    const uint32 fontIndex = (uint32)FontId::OCR_A_REGULAR_18;
    const_string text = ToString("the quick brown fox jumps over the lazy dog");
    const Vec4 textColor = Vec4::one;
    PushText(fontIndex, appState->fontFaces[fontIndex], text, Vec2Int { 100, 100 }, 0.0f, screenSize, textColor,
             &transientState->frameState.textRenderState);

    for (uint32 i = 0; i < 10; i++) {
        const Mat4 modelMob = Translate(Vec3 { 1.0f * i, 0.5f * i, 0.0f });

        // PushMesh(MeshId::MOB, modelMob, &transientState->frameState.meshRenderState);

        // PushMesh(MeshId::TILE_BACK, modelMob, &transientState->frameState.meshRenderState);
    }

    const Mat4 modelTile = Translate(Vec3 { 0.0f, 0.0f, 0.5f });
    //PushMesh(MeshId::TILE_FRONT, modelTile, &transientState->frameState.meshRenderState);
    PushMesh(MeshId::TILE_RIGHT, modelTile, &transientState->frameState.meshRenderState);
    PushMesh(MeshId::TILE_TOP, modelTile, &transientState->frameState.meshRenderState);
    //PushMesh(MeshId::TILE_BOTTOM, modelTile, &transientState->frameState.meshRenderState);

    // ================================================================================================
    // Vulkan rendering ===============================================================================
    // ================================================================================================

    VkCommandBuffer buffer = appState->vulkanAppState.commandBuffer;

    // TODO revisit this. should the platform coordinate something like this in some other way?
    // swapchain image acquisition timings seem to be kind of sloppy tbh, so this might be the best way.
    VkFence fence = appState->vulkanAppState.fence;
    if (vkWaitForFences(vulkanState.window.device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        LOG_ERROR("vkWaitForFences didn't return success for fence %lu\n", swapchainImageIndex);
    }
    if (vkResetFences(vulkanState.window.device, 1, &fence) != VK_SUCCESS) {
        LOG_ERROR("vkResetFences didn't return success for fence %lu\n", swapchainImageIndex);
    }

    if (vkResetCommandBuffer(buffer, 0) != VK_SUCCESS) {
        LOG_ERROR("vkResetCommandBuffer failed\n");
    }

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = 0;
    beginInfo.pInheritanceInfo = nullptr;

    if (vkBeginCommandBuffer(buffer, &beginInfo) != VK_SUCCESS) {
        LOG_ERROR("vkBeginCommandBuffer failed\n");
    }

    const VkClearValue clearValues[] = {
        { 0.0f, 0.0f, 0.0f, 1.0f },
        { 1.0f, 0 }
    };

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = vulkanState.swapchain.renderPass;
    renderPassInfo.framebuffer = vulkanState.swapchain.framebuffers[swapchainImageIndex];
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = vulkanState.swapchain.extent;
    renderPassInfo.clearValueCount = C_ARRAY_LENGTH(clearValues);
    renderPassInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

#if ENABLE_LIGHTMAPPED_MESH
    // Lightmapped meshes
    {
        const Mat4 model = Mat4::one;
        UploadAndSubmitLightmapMeshDrawCommands(vulkanState.window.device, buffer,
                                                appState->vulkanAppState.lightmapMeshPipeline,
                                                model, view, proj);
    }
#endif

    // Meshes
    {
        LinearAllocator allocator(transientState->scratch);
        UploadAndSubmitMeshDrawCommands(vulkanState.window.device, buffer, appState->vulkanAppState.meshPipeline,
                                        transientState->frameState.meshRenderState, view, proj, &allocator);
    }

    // Sprites
    {
        LinearAllocator allocator(transientState->scratch);
        UploadAndSubmitSpriteDrawCommands(vulkanState.window.device, buffer, appState->vulkanAppState.spritePipeline,
                                          transientState->frameState.spriteRenderState, &allocator);
    }

    // Text
    {
        LinearAllocator allocator(transientState->scratch);
        UploadAndSubmitTextDrawCommands(vulkanState.window.device, buffer, appState->vulkanAppState.textPipeline,
                                        transientState->frameState.textRenderState, &allocator);
    }

    vkCmdEndRenderPass(buffer);

    if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
        LOG_ERROR("vkEndCommandBuffer failed\n");
    }

    const VkSemaphore waitSemaphores[] = { vulkanState.window.imageAvailableSemaphore };
    const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    const VkSemaphore signalSemaphores[] = { vulkanState.window.renderFinishedSemaphore };

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = C_ARRAY_LENGTH(waitSemaphores);
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &buffer;
    submitInfo.signalSemaphoreCount = C_ARRAY_LENGTH(signalSemaphores);
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(vulkanState.window.graphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS) {
        LOG_ERROR("Failed to submit draw command buffer\n");
    }

    return true;
}

APP_LOAD_VULKAN_SWAPCHAIN_STATE_FUNCTION(AppLoadVulkanSwapchainState)
{
    LOG_INFO("Loading Vulkan swapchain-dependent app state\n");

    const VulkanWindow& window = vulkanState.window;
    const VulkanSwapchain& swapchain = vulkanState.swapchain;

    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);
    TransientState* transientState = GetTransientState(memory);
    LinearAllocator allocator(transientState->scratch);

    if (!LoadSpritePipelineSwapchain(window, swapchain, &allocator, &app->spritePipeline)) {
        LOG_ERROR("Failed to load swapchain-dependent Vulkan sprite pipeline\n");
        return false;
    }

    if (!LoadTextPipelineSwapchain(window, swapchain, &allocator, &app->textPipeline)) {
        LOG_ERROR("Failed to load swapchain-dependent Vulkan text pipeline\n");
        return false;
    }

    if (!LoadMeshPipelineSwapchain(window, swapchain, &allocator, &app->meshPipeline)) {
        LOG_ERROR("Failed to load swapchain-dependent Vulkan mesh pipeline\n");
        return false;
    }

#if ENABLE_LIGHTMAPPED_MESH
    if (!LoadLightmapMeshPipelineSwapchain(window, swapchain, &allocator, &app->lightmapMeshPipeline)) {
        LOG_ERROR("Failed to load swapchain-dependent Vulkan lightmap mesh pipeline\n");
        return false;
    }
#endif

    return true;
}

APP_UNLOAD_VULKAN_SWAPCHAIN_STATE_FUNCTION(AppUnloadVulkanSwapchainState)
{
    LOG_INFO("Unloading Vulkan swapchain-dependent app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

#if ENABLE_LIGHTMAPPED_MESH
    UnloadLightmapMeshPipelineSwapchain(device, &app->lightmapMeshPipeline);
#endif

    UnloadMeshPipelineSwapchain(device, &app->meshPipeline);

    UnloadTextPipelineSwapchain(device, &app->textPipeline);
    UnloadSpritePipelineSwapchain(device, &app->spritePipeline);
}

APP_LOAD_VULKAN_WINDOW_STATE_FUNCTION(AppLoadVulkanWindowState)
{
    LOG_INFO("Loading Vulkan window-dependent app state\n");

    const VulkanWindow& window = vulkanState.window;

    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);
    TransientState* transientState = GetTransientState(memory);
    LinearAllocator allocator(transientState->scratch);

    // Create command pool
    {
        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(window.surface, window.physicalDevice, &allocator);

        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.queueFamilyIndex = queueFamilyInfo.graphicsFamilyIndex;
        poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        if (vkCreateCommandPool(window.device, &poolCreateInfo, nullptr, &app->commandPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateCommandPool failed\n");
            return false;
        }
    }

    // Create command buffer
    {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = app->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(window.device, &allocInfo, &app->commandBuffer) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateCommandBuffers failed\n");
            return false;
        }
    }

    // Create fence
    {
        VkFenceCreateInfo fenceCreateInfo = {};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateFence(window.device, &fenceCreateInfo, nullptr, &app->fence) != VK_SUCCESS) {
            LOG_ERROR("vkCreateFence failed\n");
            return false;
        }
    }

    const bool spritePipeline = LoadSpritePipelineWindow(window, app->commandPool, &allocator, &app->spritePipeline);
    if (!spritePipeline) {
        LOG_ERROR("Failed to load window-dependent Vulkan sprite pipeline\n");
        return false;
    }

    const bool textPipeline = LoadTextPipelineWindow(window, app->commandPool, &allocator, &app->textPipeline);
    if (!textPipeline) {
        LOG_ERROR("Failed to load window-dependent Vulkan text pipeline\n");
        return false;
    }

    const bool meshPipeline = LoadMeshPipelineWindow(window, app->commandPool, &allocator, &app->meshPipeline);
    if (!meshPipeline) {
        LOG_ERROR("Failed to load Vulkan mesh pipeline\n");
        return false;
    }

#if ENABLE_LIGHTMAPPED_MESH
    const bool lightmapMeshPipeline = LoadLightmapMeshPipelineWindow(window, app->commandPool, &allocator,
                                                                     &app->lightmapMeshPipeline);
    if (!lightmapMeshPipeline) {
        LOG_ERROR("Failed to load Vulkan lightmap mesh pipeline\n");
        return false;
    }
#endif

    return true;
}

APP_UNLOAD_VULKAN_WINDOW_STATE_FUNCTION(AppUnloadVulkanWindowState)
{
    LOG_INFO("Unloading Vulkan window-dependent app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

#if ENABLE_LIGHTMAPPED_MESH
    UnloadLightmapMeshPipelineWindow(device, &app->lightmapMeshPipeline);
#endif
    UnloadMeshPipelineWindow(device, &app->meshPipeline);

    UnloadTextPipelineWindow(device, &app->textPipeline);
    UnloadSpritePipelineWindow(device, &app->spritePipeline);

    vkDestroyFence(device, app->fence, nullptr);
    vkDestroyCommandPool(device, app->commandPool, nullptr);
}

#include "lightmap.cpp"
#include "mesh.cpp"

#include <km_common/km_array.cpp>
#include <km_common/km_container.cpp>
#include <km_common/km_load_font.cpp>
#include <km_common/km_load_obj.cpp>
#include <km_common/km_memory.cpp>
#include <km_common/km_os.cpp>
#include <km_common/km_string.cpp>

#include <km_common/app/km_app.cpp>
#include <km_common/app/km_input.cpp>

#include <km_common/vulkan/km_vulkan_core.cpp>
#include <km_common/vulkan/km_vulkan_sprite.cpp>
#include <km_common/vulkan/km_vulkan_text.cpp>
#include <km_common/vulkan/km_vulkan_util.cpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>
#undef STB_SPRINTF_IMPLEMENTATION
