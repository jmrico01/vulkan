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
#define ENABLE_LIGHTMAPPED_MESH 0

/*
TODO

> block editor
> post-process pipeline for grain

*/

// Required for platform main
const char* WINDOW_NAME = "vulkan";
const int WINDOW_START_WIDTH  = 1600;
const int WINDOW_START_HEIGHT = 900;
const bool WINDOW_LOCK_CURSOR = true;
const uint64 PERMANENT_MEMORY_SIZE = MEGABYTES(1);
const uint64 TRANSIENT_MEMORY_SIZE = MEGABYTES(512);

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

void GenerateCityBlocks(uint32 streetSize, uint32 sidewalkSize, uint32 buildingSize, uint32 buildingHeight,
                        LinearAllocator* allocator, BlockGrid* blockGrid)
{
    MemSet(blockGrid, 0, sizeof(BlockGrid));

    const uint32 cityBlockSize = streetSize + sidewalkSize * 2 + buildingSize;

    Array<uint8> streetMask = allocator->NewArray<uint8>(cityBlockSize);
    for (uint32 i = 0; i < cityBlockSize; i++) {
        streetMask[i] = i < streetSize;
    }
    Array<uint8> sidewalkMask = allocator->NewArray<uint8>(cityBlockSize);
    for (uint32 i = 0; i < cityBlockSize; i++) {
        sidewalkMask[i] = (i >= streetSize && i < (streetSize + sidewalkSize))
            || (i >= (streetSize + sidewalkSize + buildingSize) && i < (streetSize + 2 * sidewalkSize + buildingSize));
    }
    Array<uint8> buildingMask = allocator->NewArray<uint8>(cityBlockSize);
    for (uint32 i = 0; i < cityBlockSize; i++) {
        buildingMask[i] = i >= (streetSize + sidewalkSize) && i < (streetSize + sidewalkSize + buildingSize);
    }

    static_assert(BLOCK_ORIGIN_Z > 0);
    for (uint32 z = 0; z < blockGrid->SIZE; z++) {
        for (uint32 y = 0; y < (*blockGrid)[z].SIZE; y++) {
            for (uint32 x = 0; x < (*blockGrid)[z][y].SIZE; x++) {
                const uint32 cityBlockX = x % cityBlockSize;
                const uint32 cityBlockY = y % cityBlockSize;

                const uint8 building = buildingMask[cityBlockX] * buildingMask[cityBlockY];
                if (z == BLOCK_ORIGIN_Z - 1) {
                    const uint8 street = streetMask[cityBlockX] + streetMask[cityBlockY];
                    const uint8 sidewalk = sidewalkMask[cityBlockX] + sidewalkMask[cityBlockY];
                    if (street) {
                        (*blockGrid)[z][y][x].id = BlockId::STREET;
                    }
                    else if (sidewalk) {
                        (*blockGrid)[z][y][x].id = BlockId::SIDEWALK;
                    }
                    else if (building) {
                        (*blockGrid)[z][y][x].id = BlockId::BUILDING;
                    }
                }
                else if (building && z - BLOCK_ORIGIN_Z < buildingHeight) {
                    (*blockGrid)[z][y][x].id = BlockId::BUILDING;
                }
            }
        }
    }
}

APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender)
{
    UNREFERENCED_PARAMETER(queue);
    UNREFERENCED_PARAMETER(audio);

    AppState* appState = GetAppState(memory);
    TransientState* transientState = GetTransientState(memory);

    const Vec2Int screenSize = {
        (int)vulkanState.swapchain.extent.width,
        (int)vulkanState.swapchain.extent.height
    };

    const float32 CAMERA_HEIGHT = 1.0f;

    // Initialize memory if necessary
    if (!memory->initialized) {
        const uint32 streetSize = 3;
        const uint32 sidewalkSize = 2;
        const uint32 buildingSize = 10;
        const uint32 buildingHeight = 3;

        {
            LinearAllocator allocator(transientState->scratch);
            GenerateCityBlocks(streetSize, sidewalkSize, buildingSize, buildingHeight, &allocator, &appState->blockGrid);
        }

        const Vec3 startPos = Vec3 { 0.0f, 0.0f, CAMERA_HEIGHT };

        appState->cameraPos = startPos;
        appState->cameraAngles = Vec2 { 0.0f, 0.0f };

        appState->noclipPos = startPos;
        appState->noclip = false;

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

    if (KeyPressed(input, KM_KEY_N)) {
        appState->noclip = !appState->noclip;
    }

    const float32 cameraSensitivity = 2.0f;
    const Vec2 mouseDeltaFrac = {
        (float32)input.mouseDelta.x / (float32)screenSize.x,
        (float32)input.mouseDelta.y / (float32)screenSize.y
    };
    appState->cameraAngles.x += mouseDeltaFrac.x * cameraSensitivity;
    appState->cameraAngles.y -= mouseDeltaFrac.y * cameraSensitivity;

    appState->cameraAngles.x = ModFloat32(appState->cameraAngles.x, PI_F * 2.0f);
    appState->cameraAngles.y = ClampFloat32(appState->cameraAngles.y, -PI_F / 2.0f, PI_F / 2.0f);

    const Quat cameraRotYaw = QuatFromAngleUnitAxis(appState->cameraAngles.x, Vec3::unitZ);
    const Quat cameraRotPitch = QuatFromAngleUnitAxis(appState->cameraAngles.y, Vec3::unitY);

    const Quat cameraRotYawInv = Inverse(cameraRotYaw);
    const Vec3 cameraForward = cameraRotYawInv * Vec3::unitX;
    const Vec3 cameraRight = cameraRotYawInv * -Vec3::unitY;
    const Vec3 cameraUp = Vec3::unitZ;

    float32 speed = 5.0f;
    if (KeyDown(input, KM_KEY_SHIFT)) {
        if (appState->noclip) {
            speed *= 3.0f;
        }
        else {
            speed *= 1.6f;
        }
    }

    Vec3 velocity = Vec3::zero;
    if (KeyDown(input, KM_KEY_W)) {
        velocity += cameraForward;
    }
    if (KeyDown(input, KM_KEY_S)) {
        velocity -= cameraForward;
    }
    if (KeyDown(input, KM_KEY_A)) {
        velocity -= cameraRight;
    }
    if (KeyDown(input, KM_KEY_D)) {
        velocity += cameraRight;
    }
    if (appState->noclip) {
        if (KeyDown(input, KM_KEY_SPACE)) {
            velocity += cameraUp;
        }
        if (KeyDown(input, KM_KEY_CTRL)) {
            velocity -= cameraUp;
        }
    }
    else {
        appState->cameraPos.z = CAMERA_HEIGHT;
    }

    if (velocity != Vec3::zero) {
        velocity = Normalize(velocity) * speed * deltaTime;
        if (appState->noclip) {
            appState->noclipPos += velocity;
        }
        else {
            appState->cameraPos += velocity;
        }
    }

    const Quat cameraRot = cameraRotPitch * cameraRotYaw;
    const Mat4 cameraRotMat4 = UnitQuatToMat4(cameraRot);

    // Transforms world-view camera (+X forward, +Z up) to Vulkan camera (+Z forward, -Y up)
    const Quat baseCameraRot = QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitY)
        * QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitX);
    const Mat4 baseCameraRotMat4 = UnitQuatToMat4(baseCameraRot);

    Mat4 view;
    if (appState->noclip) {
        view = baseCameraRotMat4 * cameraRotMat4 * Translate(-appState->noclipPos);
    }
    else {
        view = baseCameraRotMat4 * cameraRotMat4 * Translate(-appState->cameraPos);
    }

    const float32 aspect = (float32)screenSize.x / (float32)screenSize.y;
    const float32 nearZ = 0.1f;
    const float32 farZ = 100.0f;
    const Mat4 proj = Perspective(PI_F / 4.0f, aspect, nearZ, farZ);

    const uint32 fontIndex = (uint32)FontId::OCR_A_REGULAR_18;
    const_string text = ToString("the quick brown fox jumps over the lazy dog");
    const Vec4 textColor = Vec4::one;
    PushText(fontIndex, appState->fontFaces[fontIndex], text, Vec2Int { 100, 100 }, 0.0f, textColor, screenSize,
             &transientState->frameState.textRenderState);

    const Vec4 rectColor = Vec4 { 1.0f, 0.6f, 0.6f, 1.0f };
    PushSprite((uint32)SpriteId::PIXEL, Vec2Int { 100, 300 }, Vec2Int { 400, 50 }, 0.0f, rectColor, screenSize,
               &transientState->frameState.spriteRenderState);

    // Draw blocks
    {
        const Mat4 scale = Scale(Vec3 { BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE });

        const Mat4 top    = Translate(Vec3::unitZ);
        const Mat4 bottom = UnitQuatToMat4(QuatFromAngleUnitAxis(PI_F, Normalize(Vec3 { 1.0f, 1.0f, 0.0f })));
        const Mat4 left   = Translate(Vec3 { 0.0f, 1.0f, 1.0f }) *
            UnitQuatToMat4(QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitX));
        const Mat4 right  = UnitQuatToMat4(QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitX));
        const Mat4 front  = Translate(Vec3 { 1.0f, 0.0f, 1.0f }) *
            UnitQuatToMat4(QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitY));
        const Mat4 back   = UnitQuatToMat4(QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitY));

        for (uint32 z = 0; z < appState->blockGrid.SIZE; z++) {
            for (uint32 y = 0; y < appState->blockGrid[z].SIZE; y++) {
                for (uint32 x = 0; x < appState->blockGrid[z][y].SIZE; x++) {
                    const Block block = appState->blockGrid[z][y][x];

                    Vec3 color = Vec3::one;
                    switch (block.id) {
                        case BlockId::NONE: {
                            continue;
                        } break;
                        case BlockId::SIDEWALK: {
                            color = Vec3::one * 0.7f;
                        } break;
                        case BlockId::STREET: {
                            color = Vec3::one * 0.5f;
                        } break;
                        case BlockId::BUILDING: {
                            color = Vec3::one * 0.8f;
                        } break;
                    }

                    const Vec3 pos = {
                        (float32)((int)x - (int)BLOCK_ORIGIN_X) * BLOCK_SIZE,
                        (float32)((int)y - (int)BLOCK_ORIGIN_Y) * BLOCK_SIZE,
                        (float32)((int)z - (int)BLOCK_ORIGIN_Z) * BLOCK_SIZE,
                    };
                    const Mat4 posTransform = Translate(pos);
                    if (z == BLOCKS_SIZE_Z - 1 || appState->blockGrid[z + 1][y][x].id == BlockId::NONE) {
                        PushMesh(MeshId::TILE, posTransform * scale * top, color,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (z == 0 || appState->blockGrid[z - 1][y][x].id == BlockId::NONE) {
                        PushMesh(MeshId::TILE, posTransform * scale * bottom, color,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (y == BLOCKS_SIZE_Y - 1 || appState->blockGrid[z][y + 1][x].id == BlockId::NONE) {
                        PushMesh(MeshId::TILE, posTransform * scale * left, color,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (y == 0 || appState->blockGrid[z][y - 1][x].id == BlockId::NONE) {
                        PushMesh(MeshId::TILE, posTransform * scale * right, color,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (x == BLOCKS_SIZE_X - 1 || appState->blockGrid[z][y][x + 1].id == BlockId::NONE) {
                        PushMesh(MeshId::TILE, posTransform * scale * front, color,
                                 &transientState->frameState.meshRenderState);
                    }
                    if (x == 0 || appState->blockGrid[z][y][x - 1].id == BlockId::NONE) {
                        PushMesh(MeshId::TILE, posTransform * scale * back, color,
                                 &transientState->frameState.meshRenderState);
                    }
                }
            }
        }
    }

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

    AppState* appState = GetAppState(memory);
    VulkanAppState* app = &appState->vulkanAppState;
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

    // Sprites
    {
        const char* spriteFilePaths[] = {
            "data/sprites/pixel.png",
            "data/sprites/jon.png",
            "data/sprites/rock.png"
        };
        static_assert(C_ARRAY_LENGTH(spriteFilePaths) == (uint32)SpriteId::COUNT);

        for (uint32 i = 0; i < C_ARRAY_LENGTH(spriteFilePaths); i++) {
            int width, height, channels;
            unsigned char* imageData = stbi_load(spriteFilePaths[i], &width, &height, &channels, 0);
            if (imageData == NULL) {
                DEBUG_PANIC("Failed to load sprite: %s\n", spriteFilePaths[i]);
            }
            defer(stbi_image_free(imageData));

            uint8* vulkanImageData = (uint8*)imageData;
            if (channels == 3) {
                LOG_ERROR("Image %s with 3 channels, converting to RGBA\n", spriteFilePaths[i]);

                vulkanImageData = allocator.New<uint8>(width * height * 4);
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        const int imageDataInd = (y * width + x) * 3;
                        vulkanImageData[imageDataInd + 0] = imageData[imageDataInd + 0];
                        vulkanImageData[imageDataInd + 1] = imageData[imageDataInd + 1];
                        vulkanImageData[imageDataInd + 2] = imageData[imageDataInd + 2];
                        vulkanImageData[imageDataInd + 3] = 255;
                    }
                }

                channels = 4;
            }

            VulkanImage sprite;
            if (!LoadVulkanImage(window.device, window.physicalDevice, window.graphicsQueue, app->commandPool,
                                 width, height, channels, vulkanImageData, &sprite)) {
                DEBUG_PANIC("Failed to Vulkan image for sprite %s\n", spriteFilePaths[i]);
            }

            uint32 spriteIndex;
            if (!RegisterSprite(window.device, &app->spritePipeline, sprite, &spriteIndex)) {
                DEBUG_PANIC("Failed to register sprite %s\n", spriteFilePaths[i]);
            }
            DEBUG_ASSERT(spriteIndex == i);
        }
    }

    // Fonts
    {
        struct FontData {
            const_string filePath;
            uint32 height;
        };
        const FontData fontData[] = {
            { ToString("data/fonts/ocr-a/regular.ttf"), 18 },
            { ToString("data/fonts/ocr-a/regular.ttf"), 24 },
        };
        static_assert(C_ARRAY_LENGTH(fontData) == (uint32)FontId::COUNT);

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
            if (!LoadVulkanImage(window.device, window.physicalDevice, window.graphicsQueue, app->commandPool,
                                 fontFace.atlasWidth, fontFace.atlasHeight, 1, fontFace.atlasData, &fontAtlas)) {
                DEBUG_PANIC("Failed to Vulkan image for font atlas %lu\n", i);
            }

            uint32 fontIndex;
            if (!RegisterFont(window.device, &app->textPipeline, fontAtlas, &fontIndex)) {
                DEBUG_PANIC("Failed to register font %lu\n", i);
            }
            DEBUG_ASSERT(fontIndex == i);
        }
    }

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
