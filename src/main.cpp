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

struct VulkanVertex
{
    Vec3 pos;
    Vec3 normal;
    Vec3 color;
    Vec2 uv;
    float32 lightmapWeight;
};

using VulkanTriangle = StaticArray<VulkanVertex, 3>;

struct VulkanGeometry
{
    bool valid;
    Array<uint32> meshEndInds;
    Array<VulkanTriangle> triangles;
};

VulkanGeometry ObjToVulkanGeometry(const LoadObjResult& obj, LinearAllocator* allocator)
{
    VulkanGeometry geometry;
    geometry.valid = false;
    geometry.meshEndInds = allocator->NewArray<uint32>(obj.models.size);
    if (geometry.meshEndInds.data == nullptr) {
        return geometry;
    }

    uint32 totalTriangles = 0;
    for (uint32 i = 0; i < obj.models.size; i++) {
        totalTriangles += obj.models[i].triangles.size + obj.models[i].quads.size * 2;
    }
    geometry.triangles = allocator->NewArray<VulkanTriangle>(totalTriangles);
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

struct UniformBufferObject
{
    alignas(16) Mat4 model;
    alignas(16) Mat4 view;
    alignas(16) Mat4 proj;
};

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

    UniformBufferObject ubo;
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

    void* data;
    vkMapMemory(vulkanState.window.device, appState->vulkanAppState.uniformBufferMemory, 0, sizeof(ubo), 0, &data);
    MemCopy(data, &ubo, sizeof(ubo));
    vkUnmapMemory(vulkanState.window.device, appState->vulkanAppState.uniformBufferMemory);

    const VkSemaphore waitSemaphores[] = { vulkanState.window.imageAvailableSemaphore };
    const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    const VkSemaphore signalSemaphores[] = { vulkanState.window.renderFinishedSemaphore };

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = C_ARRAY_LENGTH(waitSemaphores);
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &appState->vulkanAppState.commandBuffers[(uint64)swapchainImageIndex];
    submitInfo.signalSemaphoreCount = C_ARRAY_LENGTH(signalSemaphores);
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(vulkanState.window.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
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

    // Create graphics pipeline
    {
        const Array<uint8> vertShaderCode = LoadEntireFile(ToString("data/shaders/shader.vert.spv"), &allocator);
        if (vertShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load vertex shader code\n");
            return false;
        }
        const Array<uint8> fragShaderCode = LoadEntireFile(ToString("data/shaders/shader.frag.spv"), &allocator);
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
        bindingDescription.stride = sizeof(VulkanVertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attributeDescriptions[5] = {};
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(VulkanVertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(VulkanVertex, normal);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(VulkanVertex, color);

        attributeDescriptions[3].binding = 0;
        attributeDescriptions[3].location = 3;
        attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[3].offset = offsetof(VulkanVertex, uv);

        attributeDescriptions[4].binding = 0;
        attributeDescriptions[4].location = 4;
        attributeDescriptions[4].format = VK_FORMAT_R32_SFLOAT;
        attributeDescriptions[4].offset = offsetof(VulkanVertex, lightmapWeight);

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

#if 0
        VkDynamicState dynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT
        };

        VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo = {};
        dynamicStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicStateCreateInfo.dynamicStateCount = C_ARRAY_LENGTH(dynamicStates);
        dynamicStateCreateInfo.pDynamicStates = dynamicStates;
#endif

        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
        pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutCreateInfo.setLayoutCount = 1;
        pipelineLayoutCreateInfo.pSetLayouts = &app->descriptorSetLayout;
        pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
        pipelineLayoutCreateInfo.pPushConstantRanges = nullptr;

        if (vkCreatePipelineLayout(window.device, &pipelineLayoutCreateInfo, nullptr,
                                   &app->pipelineLayout) != VK_SUCCESS) {
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
        pipelineCreateInfo.layout = app->pipelineLayout;
        pipelineCreateInfo.renderPass = swapchain.renderPass;
        pipelineCreateInfo.subpass = 0;
        pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineCreateInfo.basePipelineIndex = -1;

        if (vkCreateGraphicsPipelines(window.device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &app->graphicsPipeline) != VK_SUCCESS) {
            LOG_ERROR("vkCreateGraphicsPipeline failed\n");
            return false;
        }
    }

    // Create command buffers
    {
        app->commandBuffers.size = swapchain.framebuffers.size;

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = app->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)app->commandBuffers.size;

        if (vkAllocateCommandBuffers(window.device, &allocInfo, app->commandBuffers.data) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateCommandBuffers failed\n");
            return false;
        }

        for (uint32 i = 0; i < app->commandBuffers.size; i++) {
            const VkCommandBuffer& buffer = app->commandBuffers[i];

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = 0;
            beginInfo.pInheritanceInfo = nullptr;

            if (vkBeginCommandBuffer(buffer, &beginInfo) != VK_SUCCESS) {
                LOG_ERROR("vkBeginCommandBuffer failed for command buffer %lu\n", i);
                return false;
            }

            const VkClearValue clearValues[] = {
                { 0.0f, 0.0f, 0.0f, 1.0f },
                { 1.0f, 0 }
            };

            VkRenderPassBeginInfo renderPassInfo = {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = swapchain.renderPass;
            renderPassInfo.framebuffer = swapchain.framebuffers[i];
            renderPassInfo.renderArea.offset = { 0, 0 };
            renderPassInfo.renderArea.extent = swapchain.extent;
            renderPassInfo.clearValueCount = C_ARRAY_LENGTH(clearValues);
            renderPassInfo.pClearValues = clearValues;

            vkCmdBeginRenderPass(buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, app->graphicsPipeline);

            const VkBuffer vertexBuffers[] = { app->vertexBuffer };
            const VkDeviceSize offsets[] = { 0 };
            vkCmdBindVertexBuffers(buffer, 0, C_ARRAY_LENGTH(vertexBuffers), vertexBuffers, offsets);

            uint32 startTriangleInd = 0;
            for (uint32 j = 0; j < app->meshTriangleEndInds.size; j++) {
                vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, app->pipelineLayout, 0, 1,
                                        &app->descriptorSets[j], 0, nullptr);

                const uint32 numTriangles = app->meshTriangleEndInds[j] - startTriangleInd;
                vkCmdDraw(buffer, numTriangles * 3, 1, startTriangleInd * 3, 0);

                startTriangleInd = app->meshTriangleEndInds[j];
            }

            vkCmdEndRenderPass(buffer);

            if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
                LOG_ERROR("vkEndCommandBuffer failed for command buffer %lu\n", i);
                return false;
            }
        }
    }

    return true;
}

APP_UNLOAD_VULKAN_SWAPCHAIN_STATE_FUNCTION(AppUnloadVulkanSwapchainState)
{
    LOG_INFO("Unloading Vulkan swapchain-dependent app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

    vkFreeCommandBuffers(device, app->commandPool, app->commandBuffers.size, app->commandBuffers.data);
    app->commandBuffers.Clear();
    vkDestroyPipeline(device, app->graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(device, app->pipelineLayout, nullptr);
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
        poolCreateInfo.flags = 0;

        if (vkCreateCommandPool(window.device, &poolCreateInfo, nullptr, &app->commandPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateCommandPool failed\n");
            return false;
        }
    }

    // Load vulkan vertex geometry
    VulkanGeometry geometry;
    {
        LoadObjResult obj;
        if (!LoadObj(ToString("data/models/reference-scene-small.obj"), &obj, &allocator)) {
            LOG_ERROR("Failed to load reference scene .obj\n");
            return false;
        }

        geometry = ObjToVulkanGeometry(obj, &allocator);
        if (!geometry.valid) {
            LOG_ERROR("Failed to load Vulkan geometry from obj\n");
            return false;
        }

        // Set per-vertex lightmap weights based on triangle areas
        for (uint32 i = 0; i < geometry.triangles.size; i++) {
            VulkanTriangle& t = geometry.triangles[i];
            const float32 area = TriangleArea(t[0].pos, t[1].pos, t[2].pos);
            const float32 weight = ClampFloat32(SmoothStep(0.0f, 0.005f, area), 0.0f, 1.0f);
            for (int j = 0; j < 3; j++) {
                t[j].lightmapWeight = 1.0f;
            }
        }

        // Load vertex colors from lightmap data
        uint32 startInd = 0;
        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            const_string filePath = AllocPrintf(&allocator, "data/lightmaps/%llu.v", i);
            Array<uint8> vertexColors = LoadEntireFile(filePath, &allocator);
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
            app->meshTriangleEndInds.Append(geometry.meshEndInds[i]);
        }
    }


    // Create vertex buffer
    {
        const VkDeviceSize vertexBufferSize = geometry.triangles.size * 3 * sizeof(VulkanVertex);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for staging buffer\n");
            return false;
        }

        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          window.device, window.physicalDevice, &app->vertexBuffer, &app->vertexBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for vertex buffer\n");
            return false;
        }

        // Copy vertex data from CPU into memory-mapped staging buffer
        void* data;
        vkMapMemory(window.device, stagingBufferMemory, 0, vertexBufferSize, 0, &data);

        MemCopy(data, geometry.triangles.data, vertexBufferSize);

        vkUnmapMemory(window.device, stagingBufferMemory);

        // Copy vertex data from staging buffer into GPU vertex buffer
        CopyBuffer(window.device, app->commandPool, window.graphicsQueue,
                   stagingBuffer, app->vertexBuffer, vertexBufferSize);

        vkDestroyBuffer(window.device, stagingBuffer, nullptr);
        vkFreeMemory(window.device, stagingBufferMemory, nullptr);
    }

    // Create lightmaps
    {
        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            const char* filePath = ToCString(AllocPrintf(&allocator, "data/lightmaps/%llu.png", i), &allocator);
            int width, height, channels;
            unsigned char* imageData = stbi_load(filePath, &width, &height, &channels, 0);
            if (imageData == NULL) {
                LOG_ERROR("Failed to load lightmap: %s\n", filePath);
                return false;
            }
            defer(stbi_image_free(imageData));

            VulkanImage* lightmapImage = app->lightmaps.Append();
            if (!LoadVulkanImage(window.device, window.physicalDevice, window.graphicsQueue, app->commandPool,
                                 width, height, channels, (const uint8*)imageData, lightmapImage)) {
                LOG_ERROR("Failed to Vulkan image for lightmap %s\n", filePath);
                return false;
            }
        }
    }

    // Create texture sampler
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

        if (vkCreateSampler(window.device, &createInfo, nullptr, &app->textureSampler) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSampler failed\n");
            return false;
        }
    }

    // Create uniform buffer
    {
        VkDeviceSize uniformBufferSize = sizeof(UniformBufferObject);
        if (!CreateBuffer(uniformBufferSize,
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice, &app->uniformBuffer, &app->uniformBufferMemory)) {
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
                                        &app->descriptorSetLayout) != VK_SUCCESS) {
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

        if (vkCreateDescriptorPool(window.device, &poolInfo, nullptr, &app->descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorPool failed\n");
            return false;
        }
    }

    // Create descriptor set
    {
        FixedArray<VkDescriptorSetLayout, VulkanAppState::MAX_LIGHTMAPS> layouts;
        layouts.Clear();
        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            layouts.Append(app->descriptorSetLayout);
        }

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = app->descriptorPool;
        allocInfo.descriptorSetCount = geometry.meshEndInds.size;
        allocInfo.pSetLayouts = layouts.data;

        if (vkAllocateDescriptorSets(window.device, &allocInfo, app->descriptorSets.data) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateDescriptorSets failed\n");
            return false;
        }
        app->descriptorSets.size = geometry.meshEndInds.size;

        for (uint32 i = 0; i < geometry.meshEndInds.size; i++) {
            VkDescriptorBufferInfo bufferInfo = {};
            bufferInfo.buffer = app->uniformBuffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkDescriptorImageInfo imageInfo = {};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = app->lightmaps[i].view;
            imageInfo.sampler = app->textureSampler;

            VkWriteDescriptorSet descriptorWrites[2] = {};
            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = app->descriptorSets[i];
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].dstArrayElement = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pBufferInfo = &bufferInfo;

            descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[1].dstSet = app->descriptorSets[i];
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

APP_UNLOAD_VULKAN_WINDOW_STATE_FUNCTION(AppUnloadVulkanWindowState)
{
    LOG_INFO("Unloading Vulkan window-dependent app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

    app->descriptorSets.Clear();
    vkDestroyDescriptorPool(device, app->descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, app->descriptorSetLayout, nullptr);

    vkDestroyBuffer(device, app->uniformBuffer, nullptr);
    vkFreeMemory(device, app->uniformBufferMemory, nullptr);

    vkDestroySampler(device, app->textureSampler, nullptr);

    for (uint32 i = 0; i < app->lightmaps.size; i++) {
        vkDestroyImageView(device, app->lightmaps[i].view, nullptr);
        vkDestroyImage(device, app->lightmaps[i].image, nullptr);
        vkFreeMemory(device, app->lightmaps[i].memory, nullptr);
    }
    app->lightmaps.Clear();

    vkDestroyBuffer(device, app->vertexBuffer, nullptr);
    vkFreeMemory(device, app->vertexBufferMemory, nullptr);

    app->meshTriangleEndInds.Clear();

    vkDestroyCommandPool(device, app->commandPool, nullptr);
}

#include "lightmap.cpp"
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
