#include "main.h"

#include <intrin.h>
#include <stdio.h>

#include <stb_image.h>

#include <km_common/km_array.h>
#include <km_common/km_defines.h>
#include <km_common/km_load_obj.h>
#include <km_common/km_os.h>
#include <km_common/km_string.h>

#include "app_main.h"
#include "lightmap.h"
#include "load_font.h"

#define ENABLE_THREADS 1

// Required for platform main
const int WINDOW_START_WIDTH  = 1600;
const int WINDOW_START_HEIGHT = 900;
const uint64 PERMANENT_MEMORY_SIZE = MEGABYTES(1);
const uint64 TRANSIENT_MEMORY_SIZE = MEGABYTES(256);

bool LoadVulkanImage(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool,
                     uint32 width, uint32 height, uint32 channels, const uint8* data, VulkanImage* image)
{
    VkFormat format;
    switch (channels) {
        case 1: {
            format = VK_FORMAT_R8_SRGB;
        } break;
        case 3: {
            format = VK_FORMAT_R8G8B8_SRGB;
        } break;
        case 4: {
            format = VK_FORMAT_R8G8B8A8_SRGB;
        } break;
        default: {
            LOG_ERROR("Unsupported image number of channels: %lu\n", channels);
            return false;
        } break;
    }

    // Create image
    if (!CreateImage(device, physicalDevice, width, height, format, VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     &image->image, &image->memory)) {
        LOG_ERROR("CreateImage failed\n");
        return false;
    }

    // Copy image data using a memory-mapped staging buffer
    const VkDeviceSize imageSize = width * height * channels;
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    if (!CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      device, physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
        LOG_ERROR("CreateBuffer failed for staging buffer\n");
        return false;
    }
    defer({
              vkDestroyBuffer(device, stagingBuffer, nullptr);
              vkFreeMemory(device, stagingBufferMemory, nullptr);
          });

    void* memoryMappedData;
    vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &memoryMappedData);
    MemCopy(memoryMappedData, data, imageSize);
    vkUnmapMemory(device, stagingBufferMemory);

    TransitionImageLayout(device, commandPool, queue, image->image,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CopyBufferToImage(device, commandPool, queue, stagingBuffer, image->image, width, height);
    TransitionImageLayout(device, commandPool, queue, image->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Create image view
    if (!CreateImageView(device, image->image, format, VK_IMAGE_ASPECT_COLOR_BIT, &image->view)) {
        LOG_ERROR("CreateImageView failed\n");
        return false;
    }

    return true;
}

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

struct VulkanTextInstanceData
{
    Vec3 pos;
    Vec2 size;
    Vec4 uvInfo;
};

struct RectCoordsNdc
{
    Vec3 pos;
    Vec2 size;
};

RectCoordsNdc ToRectCoordsNdc(Vec2Int pos, Vec2Int size, Vec2Int screenSize)
{
	return RectCoordsNdc {
        .pos = {
            2.0f * pos.x / screenSize.x - 1.0f,
            2.0f * pos.y / screenSize.y - 1.0f,
            0.0f
        },
        .size = {
            2.0f * size.x / screenSize.x,
            2.0f * size.y / screenSize.y
        },
    };
}

RectCoordsNdc ToRectCoordsNdc(Vec2Int pos, Vec2Int size, Vec2 anchor, Vec2Int screenSize)
{
	RectCoordsNdc result;
	result.pos = { (float32)pos.x, (float32)pos.y, 0.0f };
	result.size = { (float32)size.x, (float32)size.y };
	result.pos.x -= anchor.x * result.size.x;
	result.pos.y -= anchor.y * result.size.y;
	result.pos.x = result.pos.x * 2.0f / screenSize.x - 1.0f;
	result.pos.y = result.pos.y * 2.0f / screenSize.y - 1.0f;
	result.size.x *= 2.0f / screenSize.x;
	result.size.y *= 2.0f / screenSize.y;
	return result;
}

void FontTextToInstanceData(const FontFace& fontFace, const_string text, Vec2Int pos, Vec2Int screenSize,
                            VulkanTextInstanceData* instanceData)
{
    Vec2Int offset = Vec2Int::zero;
    int ind = 0;
    for (uint32 i = 0; i < text.size; i++) {
        const uint32 ch = text[i];
        const GlyphInfo& glyphInfo = fontFace.glyphInfo[ch];

        const Vec2Int glyphPos = pos + offset + glyphInfo.offset;
        const RectCoordsNdc ndc = ToRectCoordsNdc(glyphPos, glyphInfo.size, screenSize);

        instanceData[ind].pos = ndc.pos;
        instanceData[ind].size = ndc.size;
        instanceData[ind].uvInfo = {
            glyphInfo.uvOrigin.x, glyphInfo.uvOrigin.y,
            glyphInfo.uvSize.x, glyphInfo.uvSize.y
        };

        offset += glyphInfo.advance / 64;
        ind++;
    }
}

struct VulkanMeshVertex
{
    Vec3 pos;
    Vec3 normal;
    Vec3 color;
    Vec2 uv;
    float32 lightmapWeight;
};

using VulkanMeshTriangle = StaticArray<VulkanMeshVertex, 3>;

struct VulkanMeshGeometry
{
    bool valid;
    Array<uint32> meshEndInds;
    Array<VulkanMeshTriangle> triangles;
};

struct MeshUniformBufferObject
{
    alignas(16) Mat4 model;
    alignas(16) Mat4 view;
    alignas(16) Mat4 proj;
};

VulkanMeshGeometry ObjToVulkanMeshGeometry(const LoadObjResult& obj, LinearAllocator* allocator)
{
    VulkanMeshGeometry geometry;
    geometry.valid = false;
    geometry.meshEndInds = allocator->NewArray<uint32>(obj.models.size);
    if (geometry.meshEndInds.data == nullptr) {
        return geometry;
    }

    uint32 totalTriangles = 0;
    for (uint32 i = 0; i < obj.models.size; i++) {
        totalTriangles += obj.models[i].triangles.size + obj.models[i].quads.size * 2;
    }
    geometry.triangles = allocator->NewArray<VulkanMeshTriangle>(totalTriangles);
    if (geometry.triangles.data == nullptr) {
        return geometry;
    }

    const Vec3 vertexColor = Vec3::zero;

    uint32 endInd = 0;
    for (uint32 i = 0; i < obj.models.size; i++) {
        for (uint32 j = 0; j < obj.models[i].triangles.size; j++) {
            const ObjTriangle& t = obj.models[i].triangles[j];
            const uint32 tInd = endInd + j;
            const Vec3 normal = CalculateTriangleUnitNormal(t.v[0].pos, t.v[1].pos, t.v[2].pos);

            for (int k = 0; k < 3; k++) {
                geometry.triangles[tInd][k].pos = t.v[k].pos;
                geometry.triangles[tInd][k].normal = normal;
                geometry.triangles[tInd][k].color = vertexColor;
                geometry.triangles[tInd][k].uv = t.v[k].uv;
            }
        }
        endInd += obj.models[i].triangles.size;

        for (uint32 j = 0; j < obj.models[i].quads.size; j++) {
            const ObjQuad& q = obj.models[i].quads[j];
            const uint32 tInd = endInd + j * 2;
            const Vec3 normal = CalculateTriangleUnitNormal(q.v[0].pos, q.v[1].pos, q.v[2].pos);

            for (int k = 0; k < 3; k++) {
                geometry.triangles[tInd][k].pos = q.v[k].pos;
                geometry.triangles[tInd][k].normal = normal;
                geometry.triangles[tInd][k].color = vertexColor;
                geometry.triangles[tInd][k].uv = q.v[k].uv;
            }

            for (int k = 0; k < 3; k++) {
                const uint32 quadInd = (k + 2) % 4;
                geometry.triangles[tInd + 1][k].pos = q.v[quadInd].pos;
                geometry.triangles[tInd + 1][k].normal = normal;
                geometry.triangles[tInd + 1][k].color = vertexColor;
                geometry.triangles[tInd + 1][k].uv = q.v[quadInd].uv;
            }
        }
        endInd += obj.models[i].quads.size * 2;

        geometry.meshEndInds[i] = endInd;
    }

    geometry.valid = true;
    return geometry;
}

internal AppState* GetAppState(AppMemory* memory)
{
    DEBUG_ASSERT(sizeof(AppState) < memory->permanent.size);
    return (AppState*)memory->permanent.data;
}

APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender)
{
    UNREFERENCED_PARAMETER(audio);

    AppState* appState = GetAppState(memory);
    const Vec2Int screenSize = {
        (int)vulkanState.swapchain.extent.width,
        (int)vulkanState.swapchain.extent.height
    };

    if (!memory->initialized) {
        appState->totalElapsed = 0.0f;
        appState->cameraPos = Vec3 { -1.0f, 0.0f, 1.0f };
        appState->cameraAngles = Vec2 { 0.0f, 0.0f };

        memory->initialized = true;
    }

    if (KeyPressed(input, KM_KEY_L)) {
        LinearAllocator allocator(memory->transient);

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

    appState->totalElapsed += deltaTime;

    MeshUniformBufferObject ubo;
    ubo.model = Mat4::one;

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

    ubo.view = baseCameraRotMat4 * cameraRotMat4 * Translate(-appState->cameraPos);

    const float32 aspect = (float32)screenSize.x / (float32)screenSize.y;
    const float32 nearZ = 0.1f;
    const float32 farZ = 50.0f;
    ubo.proj = Perspective(PI_F / 4.0f, aspect, nearZ, farZ);

    struct VulkanSpriteDrawData
    {
        SpriteId id;
        Vec3 pos;
        Vec2 size;
    };
    FixedArray<VulkanSpriteDrawData, VulkanSpritePipeline::MAX_INSTANCES> spriteDrawData;
    spriteDrawData.Clear();

    const uint32 NUM_JONS = 10;
    for (uint32 i = 0; i < NUM_JONS; i++) {
        const Vec3 pos = { RandFloat32(-1.0f, 1.0f), RandFloat32(-1.0f, 1.0f), 0.5f };
        const Vec2 size = { RandFloat32(0.1f, 0.2f), RandFloat32(0.1f, 0.2f) };
        // spriteDrawData.Append({ SpriteId::JON, pos, size });

        if (pos.x > 0.0f) {
            Vec3 rockPos = pos;
            rockPos.z = 0.2f;
            // spriteDrawData.Append({ SpriteId::ROCK, rockPos, size});
        }
    }

    // ================================================================================================
    // Vulkan rendering ===============================================================================
    // ================================================================================================

    // Copy sprite instance data to GPU
    StaticArray<uint32, VulkanSpritePipeline::MAX_SPRITES> spriteNumInstances;
    {
        const VulkanSpritePipeline& spritePipeline = appState->vulkanAppState.spritePipeline;

        LinearAllocator allocator(memory->transient);
        FixedArray<VulkanSpriteInstanceData, VulkanSpritePipeline::MAX_INSTANCES>* instanceData = allocator.New<FixedArray<VulkanSpriteInstanceData, VulkanSpritePipeline::MAX_INSTANCES>>();
        instanceData->Clear();

        for (uint32 i = 0; i < spriteNumInstances.SIZE; i++) {
            spriteNumInstances[i] = 0;

            for (uint32 j = 0; j < spriteDrawData.size; j++) {
                const uint32 spriteIndex = (uint32)spriteDrawData[j].id;
                if (spriteIndex != i) continue;

                spriteNumInstances[i]++;
                instanceData->Append({ .pos = spriteDrawData[j].pos, .size = spriteDrawData[j].size });
            }

        }

        if (instanceData->size > 0) {
            void* data;
            const uint32 bufferSize = instanceData->size * sizeof(VulkanSpriteInstanceData);
            vkMapMemory(vulkanState.window.device, spritePipeline.instanceBufferMemory, 0, bufferSize, 0, &data);
            MemCopy(data, instanceData->data, bufferSize);
            vkUnmapMemory(vulkanState.window.device, spritePipeline.instanceBufferMemory);
        }
    }

    // Copy text instance data to GPU
    StaticArray<uint32, VulkanTextPipeline::MAX_FONTS> fontNumChars;
    {
        const VulkanTextPipeline& textPipeline = appState->vulkanAppState.textPipeline;

        const FontId fontId = FontId::OCR_A_REGULAR_18;
        const_string testString = ToString("porto seguro\n\nthe quick brown fox jumps over the lazy dog");
        const Vec2Int pos = { 100, 100 };

        LinearAllocator allocator(memory->transient);
        Array<VulkanTextInstanceData> instanceData = allocator.NewArray<VulkanTextInstanceData>(testString.size);
        FontTextToInstanceData(textPipeline.fontFaces[(uint32)fontId], testString, pos, screenSize, instanceData.data);

        for (uint32 i = 0; i < fontNumChars.SIZE; i++) {
            fontNumChars[i] = i == (uint32)fontId ? testString.size : 0;
        }

        if (instanceData.size > 0) {
            void* data;
            const uint32 bufferSize = instanceData.size * sizeof(VulkanTextInstanceData);
            vkMapMemory(vulkanState.window.device, textPipeline.instanceBufferMemory, 0, bufferSize, 0, &data);
            MemCopy(data, instanceData.data, bufferSize);
            vkUnmapMemory(vulkanState.window.device, textPipeline.instanceBufferMemory);
        }
    }

    // Copy mesh uniform buffer data to GPU
    {
        const VulkanMeshPipeline& meshPipeline = appState->vulkanAppState.meshPipeline;

        void* data;
        vkMapMemory(vulkanState.window.device, meshPipeline.uniformBufferMemory,
                    0, sizeof(MeshUniformBufferObject), 0, &data);
        MemCopy(data, &ubo, sizeof(ubo));
        vkUnmapMemory(vulkanState.window.device, meshPipeline.uniformBufferMemory);
    }

    const VkSemaphore waitSemaphores[] = { vulkanState.window.imageAvailableSemaphore };
    const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    const VkSemaphore signalSemaphores[] = { vulkanState.window.renderFinishedSemaphore };

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

    // Submit commands for sprite pipeline
    {
        const VulkanSpritePipeline& spritePipeline = appState->vulkanAppState.spritePipeline;

        vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, spritePipeline.pipeline);

        const VkBuffer vertexBuffers[] = { spritePipeline.vertexBuffer, spritePipeline.instanceBuffer };
        const VkDeviceSize offsets[] = { 0, 0 };
        vkCmdBindVertexBuffers(buffer, 0, C_ARRAY_LENGTH(vertexBuffers), vertexBuffers, offsets);

        uint32 startInstance = 0;
        for (uint32 i = 0; i < spriteNumInstances.SIZE; i++) {
            vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, spritePipeline.pipelineLayout, 0, 1,
                                    &spritePipeline.descriptorSets[i], 0, nullptr);

            if (spriteNumInstances[i] > 0) {
                vkCmdDraw(buffer, 6, spriteNumInstances[i], 0, startInstance);
                startInstance += spriteNumInstances[i];
            }
        }
    }

    // Submit commands for text pipeline
    {
        const VulkanTextPipeline& textPipeline = appState->vulkanAppState.textPipeline;

        vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipeline.pipeline);

        const VkBuffer vertexBuffers[] = { textPipeline.vertexBuffer, textPipeline.instanceBuffer };
        const VkDeviceSize offsets[] = { 0, 0 };
        vkCmdBindVertexBuffers(buffer, 0, C_ARRAY_LENGTH(vertexBuffers), vertexBuffers, offsets);

        uint32 startInstance = 0;
        for (uint32 i = 0; i < fontNumChars.SIZE; i++) {
            vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipeline.pipelineLayout, 0, 1,
                                    &textPipeline.descriptorSets[i], 0, nullptr);

            if (fontNumChars[i] > 0) {
                vkCmdDraw(buffer, 6, fontNumChars[i], 0, startInstance);
                startInstance += fontNumChars[i];
            }
        }
    }

    // Submit commands for mesh pipeline
    {
        const VulkanMeshPipeline& meshPipeline = appState->vulkanAppState.meshPipeline;

        vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline.pipeline);

        const VkBuffer vertexBuffers[] = { meshPipeline.vertexBuffer };
        const VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(buffer, 0, C_ARRAY_LENGTH(vertexBuffers), vertexBuffers, offsets);

        uint32 startTriangleInd = 0;
        for (uint32 i = 0; i < meshPipeline.meshTriangleEndInds.size; i++) {
            vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline.pipelineLayout, 0, 1,
                                    &meshPipeline.descriptorSets[i], 0, nullptr);

            const uint32 numTriangles = meshPipeline.meshTriangleEndInds[i] - startTriangleInd;
            vkCmdDraw(buffer, numTriangles * 3, 1, startTriangleInd * 3, 0);

            startTriangleInd = meshPipeline.meshTriangleEndInds[i];
        }
    }

    vkCmdEndRenderPass(buffer);

    if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
        LOG_ERROR("vkEndCommandBuffer failed\n");
    }

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
    LinearAllocator allocator(memory->transient);

    // Create sprite pipeline
    {
        const Array<uint8> vertShaderCode = LoadEntireFile(ToString("data/shaders/sprite.vert.spv"), &allocator);
        if (vertShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load vertex shader code\n");
            return false;
        }
        const Array<uint8> fragShaderCode = LoadEntireFile(ToString("data/shaders/sprite.frag.spv"), &allocator);
        if (fragShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load fragment shader code\n");
            return false;
        }

        VkShaderModule vertShaderModule;
        if (!CreateShaderModule(vertShaderCode, window.device, &vertShaderModule)) {
            LOG_ERROR("Failed to create vertex shader module\n");
            return false;
        }
        defer(vkDestroyShaderModule(window.device, vertShaderModule, nullptr));

        VkShaderModule fragShaderModule;
        if (!CreateShaderModule(fragShaderCode, window.device, &fragShaderModule)) {
            LOG_ERROR("Failed to create fragment shader module\n");
            return false;
        }
        defer(vkDestroyShaderModule(window.device, fragShaderModule, nullptr));

        VkPipelineShaderStageCreateInfo vertShaderStageCreateInfo = {};
        vertShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageCreateInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageCreateInfo.module = vertShaderModule;
        vertShaderStageCreateInfo.pName = "main";
        // vertShaderStageCreateInfo.pSpecializationInfo is useful for setting shader constants

        VkPipelineShaderStageCreateInfo fragShaderStageCreateInfo = {};
        fragShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageCreateInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageCreateInfo.module = fragShaderModule;
        fragShaderStageCreateInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageCreateInfo, fragShaderStageCreateInfo };

        VkVertexInputBindingDescription bindingDescriptions[2] = {};
        VkVertexInputAttributeDescription attributeDescriptions[4] = {};

        // Per-vertex attribute bindings
        bindingDescriptions[0].binding = 0;
        bindingDescriptions[0].stride = sizeof(VulkanSpriteVertex);
        bindingDescriptions[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(VulkanSpriteVertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(VulkanSpriteVertex, uv);

        // Per-instance attribute bindings
        bindingDescriptions[1].binding = 1;
        bindingDescriptions[1].stride = sizeof(VulkanSpriteInstanceData);
        bindingDescriptions[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        attributeDescriptions[2].binding = 1;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(VulkanSpriteInstanceData, pos);

        attributeDescriptions[3].binding = 1;
        attributeDescriptions[3].location = 3;
        attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[3].offset = offsetof(VulkanSpriteInstanceData, size);

        VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo = {};
        vertexInputCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputCreateInfo.vertexBindingDescriptionCount = C_ARRAY_LENGTH(bindingDescriptions);
        vertexInputCreateInfo.pVertexBindingDescriptions = bindingDescriptions;
        vertexInputCreateInfo.vertexAttributeDescriptionCount = C_ARRAY_LENGTH(attributeDescriptions);
        vertexInputCreateInfo.pVertexAttributeDescriptions = attributeDescriptions;

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyCreateInfo = {};
        inputAssemblyCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyCreateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssemblyCreateInfo.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float32)swapchain.extent.width;
        viewport.height = (float32)swapchain.extent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = swapchain.extent;

        VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {};
        viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportStateCreateInfo.viewportCount = 1;
        viewportStateCreateInfo.pViewports = &viewport;
        viewportStateCreateInfo.scissorCount = 1;
        viewportStateCreateInfo.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizerCreateInfo = {};
        rasterizerCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizerCreateInfo.depthClampEnable = VK_FALSE;
        rasterizerCreateInfo.rasterizerDiscardEnable = VK_FALSE;
        rasterizerCreateInfo.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizerCreateInfo.lineWidth = 1.0f;
        rasterizerCreateInfo.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizerCreateInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizerCreateInfo.depthBiasEnable = VK_FALSE;
        rasterizerCreateInfo.depthBiasConstantFactor = 0.0f;
        rasterizerCreateInfo.depthBiasClamp = 0.0f;
        rasterizerCreateInfo.depthBiasSlopeFactor = 0.0f;

        VkPipelineMultisampleStateCreateInfo multisampleCreateInfo = {};
        multisampleCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleCreateInfo.sampleShadingEnable = VK_FALSE;
        multisampleCreateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampleCreateInfo.minSampleShading = 1.0f;
        multisampleCreateInfo.pSampleMask = nullptr;
        multisampleCreateInfo.alphaToCoverageEnable = VK_FALSE;
        multisampleCreateInfo.alphaToOneEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlendingCreateInfo = {};
        colorBlendingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendingCreateInfo.logicOpEnable = VK_FALSE;
        colorBlendingCreateInfo.logicOp = VK_LOGIC_OP_COPY;
        colorBlendingCreateInfo.attachmentCount = 1;
        colorBlendingCreateInfo.pAttachments = &colorBlendAttachment;
        colorBlendingCreateInfo.blendConstants[0] = 0.0f;
        colorBlendingCreateInfo.blendConstants[1] = 0.0f;
        colorBlendingCreateInfo.blendConstants[2] = 0.0f;
        colorBlendingCreateInfo.blendConstants[3] = 0.0f;

        VkPipelineDepthStencilStateCreateInfo depthStencilCreateInfo = {};
        depthStencilCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencilCreateInfo.depthTestEnable = VK_TRUE;
        depthStencilCreateInfo.depthWriteEnable = VK_TRUE;
        depthStencilCreateInfo.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencilCreateInfo.depthBoundsTestEnable = VK_FALSE;
        depthStencilCreateInfo.minDepthBounds = 0.0f; // disabled
        depthStencilCreateInfo.maxDepthBounds = 1.0f; // disabled
        depthStencilCreateInfo.stencilTestEnable = VK_FALSE;

        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
        pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCreateInfo.setLayoutCount = 1;
        pipelineLayoutCreateInfo.pSetLayouts = &app->spritePipeline.descriptorSetLayout;
        pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
        pipelineLayoutCreateInfo.pPushConstantRanges = nullptr;

        if (vkCreatePipelineLayout(window.device, &pipelineLayoutCreateInfo, nullptr,
                                   &app->spritePipeline.pipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("vkCreatePipelineLayout failed\n");
            return false;
        }

        VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
        pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineCreateInfo.stageCount = C_ARRAY_LENGTH(shaderStages);
        pipelineCreateInfo.pStages = shaderStages;
        pipelineCreateInfo.pVertexInputState = &vertexInputCreateInfo;
        pipelineCreateInfo.pInputAssemblyState = &inputAssemblyCreateInfo;
        pipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
        pipelineCreateInfo.pRasterizationState = &rasterizerCreateInfo;
        pipelineCreateInfo.pMultisampleState = &multisampleCreateInfo;
        pipelineCreateInfo.pDepthStencilState = &depthStencilCreateInfo;
        pipelineCreateInfo.pColorBlendState = &colorBlendingCreateInfo;
        pipelineCreateInfo.pDynamicState = nullptr;
        pipelineCreateInfo.layout = app->spritePipeline.pipelineLayout;
        pipelineCreateInfo.renderPass = swapchain.renderPass;
        pipelineCreateInfo.subpass = 0;
        pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineCreateInfo.basePipelineIndex = -1;

        if (vkCreateGraphicsPipelines(window.device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr,
                                      &app->spritePipeline.pipeline) != VK_SUCCESS) {
            LOG_ERROR("vkCreateGraphicsPipeline failed\n");
            return false;
        }
    }

    // Create text pipeline
    {
        const Array<uint8> vertShaderCode = LoadEntireFile(ToString("data/shaders/text.vert.spv"), &allocator);
        if (vertShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load vertex shader code\n");
            return false;
        }
        const Array<uint8> fragShaderCode = LoadEntireFile(ToString("data/shaders/text.frag.spv"), &allocator);
        if (fragShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load fragment shader code\n");
            return false;
        }

        VkShaderModule vertShaderModule;
        if (!CreateShaderModule(vertShaderCode, window.device, &vertShaderModule)) {
            LOG_ERROR("Failed to create vertex shader module\n");
            return false;
        }
        defer(vkDestroyShaderModule(window.device, vertShaderModule, nullptr));

        VkShaderModule fragShaderModule;
        if (!CreateShaderModule(fragShaderCode, window.device, &fragShaderModule)) {
            LOG_ERROR("Failed to create fragment shader module\n");
            return false;
        }
        defer(vkDestroyShaderModule(window.device, fragShaderModule, nullptr));

        VkPipelineShaderStageCreateInfo vertShaderStageCreateInfo = {};
        vertShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageCreateInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageCreateInfo.module = vertShaderModule;
        vertShaderStageCreateInfo.pName = "main";
        // vertShaderStageCreateInfo.pSpecializationInfo is useful for setting shader constants

        VkPipelineShaderStageCreateInfo fragShaderStageCreateInfo = {};
        fragShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageCreateInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageCreateInfo.module = fragShaderModule;
        fragShaderStageCreateInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageCreateInfo, fragShaderStageCreateInfo };

        VkVertexInputBindingDescription bindingDescriptions[2] = {};
        VkVertexInputAttributeDescription attributeDescriptions[5] = {};

        // Per-vertex attribute bindings
        bindingDescriptions[0].binding = 0;
        bindingDescriptions[0].stride = sizeof(VulkanSpriteVertex);
        bindingDescriptions[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(VulkanSpriteVertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(VulkanSpriteVertex, uv);

        // Per-instance attribute bindings
        bindingDescriptions[1].binding = 1;
        bindingDescriptions[1].stride = sizeof(VulkanTextInstanceData);
        bindingDescriptions[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        attributeDescriptions[2].binding = 1;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(VulkanTextInstanceData, pos);

        attributeDescriptions[3].binding = 1;
        attributeDescriptions[3].location = 3;
        attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[3].offset = offsetof(VulkanTextInstanceData, size);

        attributeDescriptions[4].binding = 1;
        attributeDescriptions[4].location = 4;
        attributeDescriptions[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributeDescriptions[4].offset = offsetof(VulkanTextInstanceData, uvInfo);

        VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo = {};
        vertexInputCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputCreateInfo.vertexBindingDescriptionCount = C_ARRAY_LENGTH(bindingDescriptions);
        vertexInputCreateInfo.pVertexBindingDescriptions = bindingDescriptions;
        vertexInputCreateInfo.vertexAttributeDescriptionCount = C_ARRAY_LENGTH(attributeDescriptions);
        vertexInputCreateInfo.pVertexAttributeDescriptions = attributeDescriptions;

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyCreateInfo = {};
        inputAssemblyCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyCreateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssemblyCreateInfo.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float32)swapchain.extent.width;
        viewport.height = (float32)swapchain.extent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = swapchain.extent;

        VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {};
        viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportStateCreateInfo.viewportCount = 1;
        viewportStateCreateInfo.pViewports = &viewport;
        viewportStateCreateInfo.scissorCount = 1;
        viewportStateCreateInfo.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizerCreateInfo = {};
        rasterizerCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizerCreateInfo.depthClampEnable = VK_FALSE;
        rasterizerCreateInfo.rasterizerDiscardEnable = VK_FALSE;
        rasterizerCreateInfo.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizerCreateInfo.lineWidth = 1.0f;
        rasterizerCreateInfo.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizerCreateInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizerCreateInfo.depthBiasEnable = VK_FALSE;
        rasterizerCreateInfo.depthBiasConstantFactor = 0.0f;
        rasterizerCreateInfo.depthBiasClamp = 0.0f;
        rasterizerCreateInfo.depthBiasSlopeFactor = 0.0f;

        VkPipelineMultisampleStateCreateInfo multisampleCreateInfo = {};
        multisampleCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleCreateInfo.sampleShadingEnable = VK_FALSE;
        multisampleCreateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampleCreateInfo.minSampleShading = 1.0f;
        multisampleCreateInfo.pSampleMask = nullptr;
        multisampleCreateInfo.alphaToCoverageEnable = VK_FALSE;
        multisampleCreateInfo.alphaToOneEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlendingCreateInfo = {};
        colorBlendingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendingCreateInfo.logicOpEnable = VK_FALSE;
        colorBlendingCreateInfo.logicOp = VK_LOGIC_OP_COPY;
        colorBlendingCreateInfo.attachmentCount = 1;
        colorBlendingCreateInfo.pAttachments = &colorBlendAttachment;
        colorBlendingCreateInfo.blendConstants[0] = 0.0f;
        colorBlendingCreateInfo.blendConstants[1] = 0.0f;
        colorBlendingCreateInfo.blendConstants[2] = 0.0f;
        colorBlendingCreateInfo.blendConstants[3] = 0.0f;

        VkPipelineDepthStencilStateCreateInfo depthStencilCreateInfo = {};
        depthStencilCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencilCreateInfo.depthTestEnable = VK_TRUE;
        depthStencilCreateInfo.depthWriteEnable = VK_TRUE;
        depthStencilCreateInfo.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencilCreateInfo.depthBoundsTestEnable = VK_FALSE;
        depthStencilCreateInfo.minDepthBounds = 0.0f; // disabled
        depthStencilCreateInfo.maxDepthBounds = 1.0f; // disabled
        depthStencilCreateInfo.stencilTestEnable = VK_FALSE;

        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
        pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCreateInfo.setLayoutCount = 1;
        pipelineLayoutCreateInfo.pSetLayouts = &app->textPipeline.descriptorSetLayout;
        pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
        pipelineLayoutCreateInfo.pPushConstantRanges = nullptr;

        if (vkCreatePipelineLayout(window.device, &pipelineLayoutCreateInfo, nullptr,
                                   &app->textPipeline.pipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("vkCreatePipelineLayout failed\n");
            return false;
        }

        VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
        pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineCreateInfo.stageCount = C_ARRAY_LENGTH(shaderStages);
        pipelineCreateInfo.pStages = shaderStages;
        pipelineCreateInfo.pVertexInputState = &vertexInputCreateInfo;
        pipelineCreateInfo.pInputAssemblyState = &inputAssemblyCreateInfo;
        pipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
        pipelineCreateInfo.pRasterizationState = &rasterizerCreateInfo;
        pipelineCreateInfo.pMultisampleState = &multisampleCreateInfo;
        pipelineCreateInfo.pDepthStencilState = &depthStencilCreateInfo;
        pipelineCreateInfo.pColorBlendState = &colorBlendingCreateInfo;
        pipelineCreateInfo.pDynamicState = nullptr;
        pipelineCreateInfo.layout = app->textPipeline.pipelineLayout;
        pipelineCreateInfo.renderPass = swapchain.renderPass;
        pipelineCreateInfo.subpass = 0;
        pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineCreateInfo.basePipelineIndex = -1;

        if (vkCreateGraphicsPipelines(window.device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr,
                                      &app->textPipeline.pipeline) != VK_SUCCESS) {
            LOG_ERROR("vkCreateGraphicsPipeline failed\n");
            return false;
        }
    }

    // Create mesh pipeline
    {
        const Array<uint8> vertShaderCode = LoadEntireFile(ToString("data/shaders/mesh.vert.spv"), &allocator);
        if (vertShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load vertex shader code\n");
            return false;
        }
        const Array<uint8> fragShaderCode = LoadEntireFile(ToString("data/shaders/mesh.frag.spv"), &allocator);
        if (fragShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load fragment shader code\n");
            return false;
        }

        VkShaderModule vertShaderModule;
        if (!CreateShaderModule(vertShaderCode, window.device, &vertShaderModule)) {
            LOG_ERROR("Failed to create vertex shader module\n");
            return false;
        }
        defer(vkDestroyShaderModule(window.device, vertShaderModule, nullptr));

        VkShaderModule fragShaderModule;
        if (!CreateShaderModule(fragShaderCode, window.device, &fragShaderModule)) {
            LOG_ERROR("Failed to create fragment shader module\n");
            return false;
        }
        defer(vkDestroyShaderModule(window.device, fragShaderModule, nullptr));

        VkPipelineShaderStageCreateInfo vertShaderStageCreateInfo = {};
        vertShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageCreateInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageCreateInfo.module = vertShaderModule;
        vertShaderStageCreateInfo.pName = "main";
        // vertShaderStageCreateInfo.pSpecializationInfo is useful for setting shader constants

        VkPipelineShaderStageCreateInfo fragShaderStageCreateInfo = {};
        fragShaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageCreateInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageCreateInfo.module = fragShaderModule;
        fragShaderStageCreateInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageCreateInfo, fragShaderStageCreateInfo };

        VkVertexInputBindingDescription bindingDescription = {};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(VulkanMeshVertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attributeDescriptions[5] = {};
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(VulkanMeshVertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(VulkanMeshVertex, normal);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(VulkanMeshVertex, color);

        attributeDescriptions[3].binding = 0;
        attributeDescriptions[3].location = 3;
        attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[3].offset = offsetof(VulkanMeshVertex, uv);

        attributeDescriptions[4].binding = 0;
        attributeDescriptions[4].location = 4;
        attributeDescriptions[4].format = VK_FORMAT_R32_SFLOAT;
        attributeDescriptions[4].offset = offsetof(VulkanMeshVertex, lightmapWeight);

        VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo = {};
        vertexInputCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputCreateInfo.vertexBindingDescriptionCount = 1;
        vertexInputCreateInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputCreateInfo.vertexAttributeDescriptionCount = C_ARRAY_LENGTH(attributeDescriptions);
        vertexInputCreateInfo.pVertexAttributeDescriptions = attributeDescriptions;

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyCreateInfo = {};
        inputAssemblyCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyCreateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssemblyCreateInfo.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float32)swapchain.extent.width;
        viewport.height = (float32)swapchain.extent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = swapchain.extent;

        VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {};
        viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportStateCreateInfo.viewportCount = 1;
        viewportStateCreateInfo.pViewports = &viewport;
        viewportStateCreateInfo.scissorCount = 1;
        viewportStateCreateInfo.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rasterizerCreateInfo = {};
        rasterizerCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizerCreateInfo.depthClampEnable = VK_FALSE;
        rasterizerCreateInfo.rasterizerDiscardEnable = VK_FALSE;
        rasterizerCreateInfo.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizerCreateInfo.lineWidth = 1.0f;
        rasterizerCreateInfo.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizerCreateInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizerCreateInfo.depthBiasEnable = VK_FALSE;
        rasterizerCreateInfo.depthBiasConstantFactor = 0.0f;
        rasterizerCreateInfo.depthBiasClamp = 0.0f;
        rasterizerCreateInfo.depthBiasSlopeFactor = 0.0f;

        VkPipelineMultisampleStateCreateInfo multisampleCreateInfo = {};
        multisampleCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleCreateInfo.sampleShadingEnable = VK_FALSE;
        multisampleCreateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        multisampleCreateInfo.minSampleShading = 1.0f;
        multisampleCreateInfo.pSampleMask = nullptr;
        multisampleCreateInfo.alphaToCoverageEnable = VK_FALSE;
        multisampleCreateInfo.alphaToOneEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlendingCreateInfo = {};
        colorBlendingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendingCreateInfo.logicOpEnable = VK_FALSE;
        colorBlendingCreateInfo.logicOp = VK_LOGIC_OP_COPY;
        colorBlendingCreateInfo.attachmentCount = 1;
        colorBlendingCreateInfo.pAttachments = &colorBlendAttachment;
        colorBlendingCreateInfo.blendConstants[0] = 0.0f;
        colorBlendingCreateInfo.blendConstants[1] = 0.0f;
        colorBlendingCreateInfo.blendConstants[2] = 0.0f;
        colorBlendingCreateInfo.blendConstants[3] = 0.0f;

        VkPipelineDepthStencilStateCreateInfo depthStencilCreateInfo = {};
        depthStencilCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencilCreateInfo.depthTestEnable = VK_TRUE;
        depthStencilCreateInfo.depthWriteEnable = VK_TRUE;
        depthStencilCreateInfo.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencilCreateInfo.depthBoundsTestEnable = VK_FALSE;
        depthStencilCreateInfo.minDepthBounds = 0.0f; // disabled
        depthStencilCreateInfo.maxDepthBounds = 1.0f; // disabled
        depthStencilCreateInfo.stencilTestEnable = VK_FALSE;

        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
        pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCreateInfo.setLayoutCount = 1;
        pipelineLayoutCreateInfo.pSetLayouts = &app->meshPipeline.descriptorSetLayout;
        pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
        pipelineLayoutCreateInfo.pPushConstantRanges = nullptr;

        if (vkCreatePipelineLayout(window.device, &pipelineLayoutCreateInfo, nullptr,
                                   &app->meshPipeline.pipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("vkCreatePipelineLayout failed\n");
            return false;
        }

        VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
        pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineCreateInfo.stageCount = C_ARRAY_LENGTH(shaderStages);
        pipelineCreateInfo.pStages = shaderStages;
        pipelineCreateInfo.pVertexInputState = &vertexInputCreateInfo;
        pipelineCreateInfo.pInputAssemblyState = &inputAssemblyCreateInfo;
        pipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
        pipelineCreateInfo.pRasterizationState = &rasterizerCreateInfo;
        pipelineCreateInfo.pMultisampleState = &multisampleCreateInfo;
        pipelineCreateInfo.pDepthStencilState = &depthStencilCreateInfo;
        pipelineCreateInfo.pColorBlendState = &colorBlendingCreateInfo;
        pipelineCreateInfo.pDynamicState = nullptr;
        pipelineCreateInfo.layout = app->meshPipeline.pipelineLayout;
        pipelineCreateInfo.renderPass = swapchain.renderPass;
        pipelineCreateInfo.subpass = 0;
        pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineCreateInfo.basePipelineIndex = -1;

        if (vkCreateGraphicsPipelines(window.device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr,
                                      &app->meshPipeline.pipeline) != VK_SUCCESS) {
            LOG_ERROR("vkCreateGraphicsPipeline failed\n");
            return false;
        }
    }

    return true;
}

APP_UNLOAD_VULKAN_SWAPCHAIN_STATE_FUNCTION(AppUnloadVulkanSwapchainState)
{
    LOG_INFO("Unloading Vulkan swapchain-dependent app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

    vkDestroyPipeline(device, app->meshPipeline.pipeline, nullptr);
    vkDestroyPipelineLayout(device, app->meshPipeline.pipelineLayout, nullptr);

    vkDestroyPipeline(device, app->textPipeline.pipeline, nullptr);
    vkDestroyPipelineLayout(device, app->textPipeline.pipelineLayout, nullptr);

    vkDestroyPipeline(device, app->spritePipeline.pipeline, nullptr);
    vkDestroyPipelineLayout(device, app->spritePipeline.pipelineLayout, nullptr);
}

bool LoadSpritePipeline(const VulkanWindow& window, VkCommandPool commandPool, LinearAllocator* allocator,
                        VulkanSpritePipeline* spritePipeline)
{
    UNREFERENCED_PARAMETER(allocator);

    // Create vertex buffer
    {
        const VulkanSpriteVertex VERTICES[] = {
            { { 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 1.0f, 0.0f }, { 1.0f, 0.0f } },
            { { 1.0f, 1.0f }, { 1.0f, 1.0f } },

            { { 1.0f, 1.0f }, { 1.0f, 1.0f } },
            { { 0.0f, 1.0f }, { 0.0f, 1.0f } },
            { { 0.0f, 0.0f }, { 0.0f, 0.0f } },
        };

        const VkDeviceSize vertexBufferSize = C_ARRAY_LENGTH(VERTICES) * sizeof(VulkanSpriteVertex);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for staging buffer\n");
            return false;
        }

        // Copy vertex data from CPU into memory-mapped staging buffer
        void* data;
        vkMapMemory(window.device, stagingBufferMemory, 0, vertexBufferSize, 0, &data);

        MemCopy(data, VERTICES, vertexBufferSize);

        vkUnmapMemory(window.device, stagingBufferMemory);

        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          window.device, window.physicalDevice,
                          &spritePipeline->vertexBuffer, &spritePipeline->vertexBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for vertex buffer\n");
            return false;
        }

        // Copy vertex data from staging buffer into GPU vertex buffer
        CopyBuffer(window.device, commandPool, window.graphicsQueue,
                   stagingBuffer, spritePipeline->vertexBuffer, vertexBufferSize);

        vkDestroyBuffer(window.device, stagingBuffer, nullptr);
        vkFreeMemory(window.device, stagingBufferMemory, nullptr);
    }

    // Create instance buffer
    {
        const VkDeviceSize bufferSize = VulkanSpritePipeline::MAX_INSTANCES * sizeof(VulkanSpriteInstanceData);

        if (!CreateBuffer(bufferSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice,
                          &spritePipeline->instanceBuffer, &spritePipeline->instanceBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for instance buffer\n");
            return false;
        }
    }

    // Create sprites
    {
        const char* spriteFilePaths[] = {
            "data/sprites/jon.png",
            "data/sprites/rock.png"
        };

        for (int i = 0; i < C_ARRAY_LENGTH(spriteFilePaths); i++) {
            int width, height, channels;
            unsigned char* imageData = stbi_load(spriteFilePaths[i], &width, &height, &channels, 0);
            if (imageData == NULL) {
                LOG_ERROR("Failed to load sprite: %s\n", spriteFilePaths[i]);
                return false;
            }
            defer(stbi_image_free(imageData));

            if (!LoadVulkanImage(window.device, window.physicalDevice, window.graphicsQueue, commandPool, width, height,
                                 channels, (const uint8*)imageData, &spritePipeline->sprites[i])) {
                LOG_ERROR("Failed to Vulkan image for sprite %s\n", spriteFilePaths[i]);
                return false;
            }
        }
    }

    // Create texture sampler
    {
        VkSamplerCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        createInfo.magFilter = VK_FILTER_NEAREST;
        createInfo.minFilter = VK_FILTER_NEAREST;
        createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.anisotropyEnable = VK_FALSE;
        createInfo.maxAnisotropy = 1.0f;
        createInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        createInfo.unnormalizedCoordinates = VK_FALSE;
        createInfo.compareEnable = VK_FALSE;
        createInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        createInfo.mipLodBias = 0.0f;
        createInfo.minLod = 0.0f;
        createInfo.maxLod = 0.0f;

        if (vkCreateSampler(window.device, &createInfo, nullptr, &spritePipeline->spriteSampler) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSampler failed\n");
            return false;
        }
    }

    // Create descriptor set layout
    {
        VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
        samplerLayoutBinding.binding = 0;
        samplerLayoutBinding.descriptorCount = 1;
        samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerLayoutBinding.pImmutableSamplers = nullptr;
        samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCreateInfo = {};
        layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCreateInfo.bindingCount = 1;
        layoutCreateInfo.pBindings = &samplerLayoutBinding;

        if (vkCreateDescriptorSetLayout(window.device, &layoutCreateInfo, nullptr,
                                        &spritePipeline->descriptorSetLayout) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorSetLayout failed\n");
            return false;
        }
    }

    // Create descriptor pool
    {
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = VulkanSpritePipeline::MAX_SPRITES;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = VulkanSpritePipeline::MAX_SPRITES;

        if (vkCreateDescriptorPool(window.device, &poolInfo, nullptr, &spritePipeline->descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorPool failed\n");
            return false;
        }
    }

    // Create descriptor set
    {
        FixedArray<VkDescriptorSetLayout, VulkanSpritePipeline::MAX_SPRITES> layouts;
        layouts.size = VulkanSpritePipeline::MAX_SPRITES;
        for (uint32 i = 0; i < VulkanSpritePipeline::MAX_SPRITES; i++) {
            layouts[i] = spritePipeline->descriptorSetLayout;
        }

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = spritePipeline->descriptorPool;
        allocInfo.descriptorSetCount = VulkanSpritePipeline::MAX_SPRITES;
        allocInfo.pSetLayouts = layouts.data;

        if (vkAllocateDescriptorSets(window.device, &allocInfo, spritePipeline->descriptorSets) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateDescriptorSets failed\n");
            return false;
        }

        for (uint32 i = 0; i < VulkanSpritePipeline::MAX_SPRITES; i++) {
            VkDescriptorImageInfo imageInfo = {};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = spritePipeline->sprites[i].view;
            imageInfo.sampler = spritePipeline->spriteSampler;

            VkWriteDescriptorSet descriptorWrite = {};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = spritePipeline->descriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(window.device, 1, &descriptorWrite, 0, nullptr);
        }
    }

    return true;
}

void UnloadSpritePipeline(VkDevice device, VulkanSpritePipeline* spritePipeline)
{
    vkDestroyDescriptorPool(device, spritePipeline->descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, spritePipeline->descriptorSetLayout, nullptr);

    vkDestroySampler(device, spritePipeline->spriteSampler, nullptr);

    for (uint32 i = 0; i < VulkanSpritePipeline::MAX_SPRITES; i++) {
        vkDestroyImageView(device, spritePipeline->sprites[i].view, nullptr);
        vkDestroyImage(device, spritePipeline->sprites[i].image, nullptr);
        vkFreeMemory(device, spritePipeline->sprites[i].memory, nullptr);
    }

    vkDestroyBuffer(device, spritePipeline->instanceBuffer, nullptr);
    vkFreeMemory(device, spritePipeline->instanceBufferMemory, nullptr);

    vkDestroyBuffer(device, spritePipeline->vertexBuffer, nullptr);
    vkFreeMemory(device, spritePipeline->vertexBufferMemory, nullptr);
}

bool LoadTextPipeline(const VulkanWindow& window, VkCommandPool commandPool, LinearAllocator* allocator,
                      VulkanTextPipeline* textPipeline)
{
    UNREFERENCED_PARAMETER(allocator);

    // Create vertex buffer
    {
        // NOTE: text pipeline uses the same vertex data format as the sprite pipeline
        const VulkanSpriteVertex VERTICES[] = {
            { { 0.0f,  0.0f }, { 0.0f, 1.0f } },
            { { 0.0f, -1.0f }, { 0.0f, 0.0f } },
            { { 1.0f, -1.0f }, { 1.0f, 0.0f } },

            { { 1.0f, -1.0f }, { 1.0f, 0.0f } },
            { { 1.0f,  0.0f }, { 1.0f, 1.0f } },
            { { 0.0f,  0.0f }, { 0.0f, 1.0f } },
        };

        const VkDeviceSize vertexBufferSize = C_ARRAY_LENGTH(VERTICES) * sizeof(VulkanSpriteVertex);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for staging buffer\n");
            return false;
        }

        // Copy vertex data from CPU into memory-mapped staging buffer
        void* data;
        vkMapMemory(window.device, stagingBufferMemory, 0, vertexBufferSize, 0, &data);

        MemCopy(data, VERTICES, vertexBufferSize);

        vkUnmapMemory(window.device, stagingBufferMemory);

        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          window.device, window.physicalDevice,
                          &textPipeline->vertexBuffer, &textPipeline->vertexBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for vertex buffer\n");
            return false;
        }

        // Copy vertex data from staging buffer into GPU vertex buffer
        CopyBuffer(window.device, commandPool, window.graphicsQueue,
                   stagingBuffer, textPipeline->vertexBuffer, vertexBufferSize);

        vkDestroyBuffer(window.device, stagingBuffer, nullptr);
        vkFreeMemory(window.device, stagingBufferMemory, nullptr);
    }

    // Create instance buffer
    {
        const VkDeviceSize bufferSize = VulkanTextPipeline::MAX_CHARACTERS * sizeof(VulkanTextInstanceData);

        if (!CreateBuffer(bufferSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice,
                          &textPipeline->instanceBuffer, &textPipeline->instanceBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for instance buffer\n");
            return false;
        }
    }

    // Load font faces
    StaticArray<LoadFontFaceResult, VulkanTextPipeline::MAX_FONTS> fontFaces;
    {
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
            LOG_ERROR("FreeType init error: %d\n", error);
            return false;
        }

        for (uint32 i = 0; i < C_ARRAY_LENGTH(fontData); i++) {
            if (!LoadFontFace(ftLibrary, fontData[i].filePath, fontData[i].height, allocator, &fontFaces[i])) {
                LOG_ERROR("Failed to load font face at %.*s\n", fontData[i].filePath.size, fontData[i].filePath.data);
                return false;
            }
        }

        for (uint32 i = 0; i < fontFaces.SIZE; i++) {
            textPipeline->fontFaces[i].height = fontFaces[i].height;
            textPipeline->fontFaces[i].glyphInfo.FromArray(fontFaces[i].glyphInfo);
        }
    }

    // Create atlases
    {
        for (uint32 i = 0; i < fontFaces.SIZE; i++) {
            if (!LoadVulkanImage(window.device, window.physicalDevice, window.graphicsQueue, commandPool,
                                 fontFaces[i].atlasWidth, fontFaces[i].atlasHeight, 1, fontFaces[i].atlasData,
                                 &textPipeline->atlases[i])) {
                LOG_ERROR("Failed to Vulkan image for font atlas %lu\n", i);
                return false;
            }
        }
    }

    // Create texture sampler
    {
        VkSamplerCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        createInfo.magFilter = VK_FILTER_LINEAR;
        createInfo.minFilter = VK_FILTER_LINEAR;
        createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.anisotropyEnable = VK_FALSE;
        createInfo.maxAnisotropy = 1.0f;
        createInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        createInfo.unnormalizedCoordinates = VK_FALSE;
        createInfo.compareEnable = VK_FALSE;
        createInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        createInfo.mipLodBias = 0.0f;
        createInfo.minLod = 0.0f;
        createInfo.maxLod = 0.0f;

        if (vkCreateSampler(window.device, &createInfo, nullptr, &textPipeline->atlasSampler) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSampler failed\n");
            return false;
        }
    }

    // Create descriptor set layout
    {
        VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
        samplerLayoutBinding.binding = 0;
        samplerLayoutBinding.descriptorCount = 1;
        samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerLayoutBinding.pImmutableSamplers = nullptr;
        samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCreateInfo = {};
        layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCreateInfo.bindingCount = 1;
        layoutCreateInfo.pBindings = &samplerLayoutBinding;

        if (vkCreateDescriptorSetLayout(window.device, &layoutCreateInfo, nullptr,
                                        &textPipeline->descriptorSetLayout) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorSetLayout failed\n");
            return false;
        }
    }

    // Create descriptor pool
    {
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = VulkanSpritePipeline::MAX_SPRITES;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = VulkanSpritePipeline::MAX_SPRITES;

        if (vkCreateDescriptorPool(window.device, &poolInfo, nullptr, &textPipeline->descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorPool failed\n");
            return false;
        }
    }

    // Create descriptor set
    {
        FixedArray<VkDescriptorSetLayout, VulkanSpritePipeline::MAX_SPRITES> layouts;
        layouts.size = VulkanSpritePipeline::MAX_SPRITES;
        for (uint32 i = 0; i < VulkanSpritePipeline::MAX_SPRITES; i++) {
            layouts[i] = textPipeline->descriptorSetLayout;
        }

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = textPipeline->descriptorPool;
        allocInfo.descriptorSetCount = VulkanSpritePipeline::MAX_SPRITES;
        allocInfo.pSetLayouts = layouts.data;

        if (vkAllocateDescriptorSets(window.device, &allocInfo, textPipeline->descriptorSets) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateDescriptorSets failed\n");
            return false;
        }

        for (uint32 i = 0; i < VulkanSpritePipeline::MAX_SPRITES; i++) {
            VkDescriptorImageInfo imageInfo = {};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = textPipeline->atlases[i].view;
            imageInfo.sampler = textPipeline->atlasSampler;

            VkWriteDescriptorSet descriptorWrite = {};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = textPipeline->descriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(window.device, 1, &descriptorWrite, 0, nullptr);
        }
    }

    return true;
}

void UnloadTextPipeline(VkDevice device, VulkanTextPipeline* textPipeline)
{
    vkDestroyDescriptorPool(device, textPipeline->descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, textPipeline->descriptorSetLayout, nullptr);

    vkDestroySampler(device, textPipeline->atlasSampler, nullptr);

    for (uint32 i = 0; i < VulkanTextPipeline::MAX_FONTS; i++) {
        vkDestroyImageView(device, textPipeline->atlases[i].view, nullptr);
        vkDestroyImage(device, textPipeline->atlases[i].image, nullptr);
        vkFreeMemory(device, textPipeline->atlases[i].memory, nullptr);
    }

    vkDestroyBuffer(device, textPipeline->instanceBuffer, nullptr);
    vkFreeMemory(device, textPipeline->instanceBufferMemory, nullptr);

    vkDestroyBuffer(device, textPipeline->vertexBuffer, nullptr);
    vkFreeMemory(device, textPipeline->vertexBufferMemory, nullptr);
}

bool LoadMeshPipeline(const VulkanWindow& window, VkCommandPool commandPool, LinearAllocator* allocator,
                      VulkanMeshPipeline* meshPipeline)
{
    // Load vulkan vertex geometry
    VulkanMeshGeometry geometry;
    {
        LoadObjResult obj;
        if (!LoadObj(ToString("data/models/reference-scene-small.obj"), &obj, allocator)) {
            LOG_ERROR("Failed to load reference scene .obj\n");
            return false;
        }

        geometry = ObjToVulkanMeshGeometry(obj, allocator);
        if (!geometry.valid) {
            LOG_ERROR("Failed to load Vulkan geometry from obj\n");
            return false;
        }

        // Set per-vertex lightmap weights based on triangle areas
        for (uint32 i = 0; i < geometry.triangles.size; i++) {
            VulkanMeshTriangle& t = geometry.triangles[i];
            const float32 area = TriangleArea(t[0].pos, t[1].pos, t[2].pos);
            const float32 weight = ClampFloat32(SmoothStep(0.0f, 0.005f, area), 0.0f, 1.0f);
            for (int j = 0; j < 3; j++) {
                t[j].lightmapWeight = 1.0f;
            }
        }

        // Load vertex colors from lightmap data
        uint32 startInd = 0;
        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            const_string filePath = AllocPrintf(allocator, "data/lightmaps/%llu.v", i);
            Array<uint8> vertexColors = LoadEntireFile(filePath, allocator);
            if (vertexColors.data == nullptr) {
                LOG_ERROR("Failed to load vertex colors for mesh %lu\n", i);
                return false;
            }

            const bool sizeEvenVec3 = vertexColors.size % sizeof(Vec3) == 0;
            const bool sizeEvenTriangles = (vertexColors.size / sizeof(Vec3)) % 3 == 0;
            if (!sizeEvenVec3 || !sizeEvenTriangles) {
                LOG_ERROR("Incorrect format for vertex colors at %.*s, mesh %lu\n", filePath.size, filePath.data, i);
                return false;
            }

            const uint32 expectedColors = (geometry.meshEndInds[i] - startInd) * 3;
            const uint32 numColors = vertexColors.size / sizeof(Vec3);
            if (expectedColors != numColors) {
                LOG_ERROR("Mismatched number of vertex colors, expected %lu, got %lu\n", expectedColors, numColors);
                return false;
            }

            const Array<Vec3> colors = {
                .size = vertexColors.size / sizeof(Vec3),
                .data = (Vec3*)vertexColors.data
            };
            for (uint32 j = startInd; j < geometry.meshEndInds[i]; j++) {
                const uint32 colorInd = (j - startInd) * 3;
                geometry.triangles[j][0].color = colors[colorInd];
                geometry.triangles[j][1].color = colors[colorInd + 1];
                geometry.triangles[j][2].color = colors[colorInd + 2];
            }

            startInd = geometry.meshEndInds[i];
        }

        // Save mesh triangle end inds to VulkanApp structure for draw commands to use
        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            meshPipeline->meshTriangleEndInds.Append(geometry.meshEndInds[i]);
        }
    }

    // Create vertex buffer
    {
        const VkDeviceSize vertexBufferSize = geometry.triangles.size * 3 * sizeof(VulkanMeshVertex);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for staging buffer\n");
            return false;
        }

        // Copy vertex data from CPU into memory-mapped staging buffer
        void* data;
        vkMapMemory(window.device, stagingBufferMemory, 0, vertexBufferSize, 0, &data);

        MemCopy(data, geometry.triangles.data, vertexBufferSize);

        vkUnmapMemory(window.device, stagingBufferMemory);

        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          window.device, window.physicalDevice,
                          &meshPipeline->vertexBuffer, &meshPipeline->vertexBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for vertex buffer\n");
            return false;
        }

        // Copy vertex data from staging buffer into GPU vertex buffer
        CopyBuffer(window.device, commandPool, window.graphicsQueue,
                   stagingBuffer, meshPipeline->vertexBuffer, vertexBufferSize);

        vkDestroyBuffer(window.device, stagingBuffer, nullptr);
        vkFreeMemory(window.device, stagingBufferMemory, nullptr);
    }

    // Create lightmaps
    {
        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            const char* filePath = ToCString(AllocPrintf(allocator, "data/lightmaps/%llu.png", i), allocator);
            int width, height, channels;
            unsigned char* imageData = stbi_load(filePath, &width, &height, &channels, 0);
            if (imageData == NULL) {
                LOG_ERROR("Failed to load lightmap: %s\n", filePath);
                return false;
            }
            defer(stbi_image_free(imageData));

            VulkanImage* lightmapImage = meshPipeline->lightmaps.Append();
            if (!LoadVulkanImage(window.device, window.physicalDevice, window.graphicsQueue, commandPool,
                                 width, height, channels, (const uint8*)imageData, lightmapImage)) {
                LOG_ERROR("Failed to Vulkan image for lightmap %s\n", filePath);
                return false;
            }
        }
    }

    // Create lightmap sampler
    {
        VkSamplerCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        createInfo.magFilter = LIGHTMAP_TEXTURE_FILTER;
        createInfo.minFilter = LIGHTMAP_TEXTURE_FILTER;
        createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        createInfo.anisotropyEnable = VK_FALSE;
        createInfo.maxAnisotropy = 1.0f;
        createInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        createInfo.unnormalizedCoordinates = VK_FALSE;
        createInfo.compareEnable = VK_FALSE;
        createInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        createInfo.mipLodBias = 0.0f;
        createInfo.minLod = 0.0f;
        createInfo.maxLod = 0.0f;

        if (vkCreateSampler(window.device, &createInfo, nullptr, &meshPipeline->lightmapSampler) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSampler failed\n");
            return false;
        }
    }

    // Create uniform buffer
    {
        VkDeviceSize uniformBufferSize = sizeof(MeshUniformBufferObject);
        if (!CreateBuffer(uniformBufferSize,
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice,
                          &meshPipeline->uniformBuffer, &meshPipeline->uniformBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for uniform buffer\n");
            return false;
        }
    }

    // Create descriptor set layout
    {
        VkDescriptorSetLayoutBinding uboLayoutBinding = {};
        uboLayoutBinding.binding = 0;
        uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboLayoutBinding.descriptorCount = 1;
        uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        uboLayoutBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
        samplerLayoutBinding.binding = 1;
        samplerLayoutBinding.descriptorCount = 1;
        samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerLayoutBinding.pImmutableSamplers = nullptr;
        samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        const VkDescriptorSetLayoutBinding bindings[] = { uboLayoutBinding, samplerLayoutBinding };

        VkDescriptorSetLayoutCreateInfo layoutCreateInfo = {};
        layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCreateInfo.bindingCount = C_ARRAY_LENGTH(bindings);
        layoutCreateInfo.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(window.device, &layoutCreateInfo, nullptr,
                                        &meshPipeline->descriptorSetLayout) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorSetLayout failed\n");
            return false;
        }
    }

    // Create descriptor pool
    {
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = geometry.meshEndInds.size;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = geometry.meshEndInds.size;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = C_ARRAY_LENGTH(poolSizes);
        poolInfo.pPoolSizes = poolSizes;
        poolInfo.maxSets = geometry.meshEndInds.size;

        if (vkCreateDescriptorPool(window.device, &poolInfo, nullptr, &meshPipeline->descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorPool failed\n");
            return false;
        }
    }

    // Create descriptor set
    {
        FixedArray<VkDescriptorSetLayout, VulkanMeshPipeline::MAX_LIGHTMAPS> layouts;
        layouts.Clear();
        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            layouts.Append(meshPipeline->descriptorSetLayout);
        }

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = meshPipeline->descriptorPool;
        allocInfo.descriptorSetCount = geometry.meshEndInds.size;
        allocInfo.pSetLayouts = layouts.data;

        if (vkAllocateDescriptorSets(window.device, &allocInfo, meshPipeline->descriptorSets.data) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateDescriptorSets failed\n");
            return false;
        }
        meshPipeline->descriptorSets.size = geometry.meshEndInds.size;

        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            VkWriteDescriptorSet descriptorWrites[2] = {};

            VkDescriptorBufferInfo bufferInfo = {};
            bufferInfo.buffer = meshPipeline->uniformBuffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(MeshUniformBufferObject);

            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = meshPipeline->descriptorSets[i];
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].dstArrayElement = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pBufferInfo = &bufferInfo;

            VkDescriptorImageInfo imageInfo = {};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = meshPipeline->lightmaps[i].view;
            imageInfo.sampler = meshPipeline->lightmapSampler;

            descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[1].dstSet = meshPipeline->descriptorSets[i];
            descriptorWrites[1].dstBinding = 1;
            descriptorWrites[1].dstArrayElement = 0;
            descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[1].descriptorCount = 1;
            descriptorWrites[1].pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(window.device, C_ARRAY_LENGTH(descriptorWrites), descriptorWrites, 0, nullptr);
        }
    }

    return true;
}

void UnloadMeshPipeline(VkDevice device, VulkanMeshPipeline* meshPipeline)
{
    meshPipeline->descriptorSets.Clear();
    vkDestroyDescriptorPool(device, meshPipeline->descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, meshPipeline->descriptorSetLayout, nullptr);

    vkDestroyBuffer(device, meshPipeline->uniformBuffer, nullptr);
    vkFreeMemory(device, meshPipeline->uniformBufferMemory, nullptr);

    vkDestroySampler(device, meshPipeline->lightmapSampler, nullptr);

    for (uint32 i = 0; i < meshPipeline->lightmaps.size; i++) {
        vkDestroyImageView(device, meshPipeline->lightmaps[i].view, nullptr);
        vkDestroyImage(device, meshPipeline->lightmaps[i].image, nullptr);
        vkFreeMemory(device, meshPipeline->lightmaps[i].memory, nullptr);
    }
    meshPipeline->lightmaps.Clear();

    vkDestroyBuffer(device, meshPipeline->vertexBuffer, nullptr);
    vkFreeMemory(device, meshPipeline->vertexBufferMemory, nullptr);

    meshPipeline->meshTriangleEndInds.Clear();
}

APP_LOAD_VULKAN_WINDOW_STATE_FUNCTION(AppLoadVulkanWindowState)
{
    LOG_INFO("Loading Vulkan window-dependent app state\n");

    const VulkanWindow& window = vulkanState.window;

    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);
    LinearAllocator allocator(memory->transient);

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

    const bool spritePipeline = LoadSpritePipeline(window, app->commandPool, &allocator, &app->spritePipeline);
    if (!spritePipeline) {
        LOG_ERROR("Failed to load Vulkan sprite pipeline\n");
        return false;
    }

    const bool textPipeline = LoadTextPipeline(window, app->commandPool, &allocator, &app->textPipeline);
    if (!textPipeline) {
        LOG_ERROR("Failed to load Vulkan text pipeline\n");
        return false;
    }

    const bool meshPipeline = LoadMeshPipeline(window, app->commandPool, &allocator, &app->meshPipeline);
    if (!meshPipeline) {
        LOG_ERROR("Failed to load Vulkan mesh pipeline\n");
        return false;
    }

    return true;
}

APP_UNLOAD_VULKAN_WINDOW_STATE_FUNCTION(AppUnloadVulkanWindowState)
{
    LOG_INFO("Unloading Vulkan window-dependent app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

    UnloadSpritePipeline(device, &app->spritePipeline);
    UnloadTextPipeline(device, &app->textPipeline);
    UnloadMeshPipeline(device, &app->meshPipeline);

    vkDestroyFence(device, app->fence, nullptr);
    vkDestroyCommandPool(device, app->commandPool, nullptr);
}

#include "lightmap.cpp"
#include "load_font.cpp"
#include "vulkan.cpp"

#if GAME_WIN32
#include "win32_main.cpp"
#else
#error "Unsupported platform"
#endif

#include <km_common/km_array.cpp>
#include <km_common/km_container.cpp>
#include <km_common/km_input.cpp>
#include <km_common/km_load_obj.cpp>
#include <km_common/km_memory.cpp>
#include <km_common/km_os.cpp>
#include <km_common/km_string.cpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>
#undef STB_SPRINTF_IMPLEMENTATION
