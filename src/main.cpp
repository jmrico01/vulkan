#include "main.h"

#include <stdio.h>

#include <stb_image.h>

#include <km_common/km_array.h>
#include <km_common/km_defines.h>
#include <km_common/km_os.h>
#include <km_common/km_string.h>

#include "app_main.h"
#include "load_obj.h"

const int WINDOW_START_WIDTH  = 1600;
const int WINDOW_START_HEIGHT = 900;

const uint64 PERMANENT_MEMORY_SIZE = MEGABYTES(1);
const uint64 TRANSIENT_MEMORY_SIZE = MEGABYTES(32);

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

internal Vec3 PixelToModelPosition(int x, int y, int squareSize, Array<Vertex> vertices)
{
    DEBUG_ASSERT(vertices.size % 3 == 0);

    const Vec2 pixelUv = { (float32)x / squareSize, (float32)y / squareSize };

    int minDistTriangle = -1;
    float32 minDist = 0.0f;
    Vec3 minB = Vec3::zero;
    const int numTriangles = (int)(vertices.size / 3);
    for (int i = 0; i < numTriangles; i++) {
        const Vertex v1 = vertices[i * 3];
        const Vertex v2 = vertices[i * 3 + 1];
        const Vertex v3 = vertices[i * 3 + 2];

        const Vec3 b = BarycentricCoordinates(pixelUv, v1.uv, v2.uv, v3.uv);
        bool inside = true;
        float32 dist = 0.0f;
        if (0.0f > b.x || b.x > 1.0f) {
            inside = false;
            dist += b.x;
        }
        if (0.0f > b.y || b.y > 1.0f) {
            inside = false;
            dist += b.y;
        }
        if (0.0f > b.z || b.z > 1.0f) {
            inside = false;
            dist += b.z;
        }
        if (inside) {
            minDistTriangle = i;
            minB = b;
            break;
        }
        else if (minDistTriangle == -1 || dist < minDist) {
            minDistTriangle = i;
            minDist = dist;
            minB = b;
        }
    }

    DEBUG_ASSERT(minDistTriangle != -1);
    const Vec3 pos1 = vertices[minDistTriangle * 3].pos;
    const Vec3 pos2 = vertices[minDistTriangle * 3 + 1].pos;
    const Vec3 pos3 = vertices[minDistTriangle * 3 + 2].pos;
    return pos1 * minB.x + pos2 * minB.y + pos3 * minB.z;
}

internal void CalculateLightmapForModel(Array<ObjModel> models, int modelInd, int squareSize, uint32* pixels)
{
    const ObjModel& model = models[modelInd];
    DEBUG_ASSERT(model.vertices.size % 3 == 0);

    MemSet(pixels, 0, squareSize * squareSize * sizeof(uint32));

    const uint64 numTriangles = model.vertices.size / 3;
    for (uint64 i = 0; i < numTriangles; i++) {
        const Vertex v1 = model.vertices[i * 3];
        const Vertex v2 = model.vertices[i * 3 + 1];
        const Vertex v3 = model.vertices[i * 3 + 2];

        const Vec2 min = {
            MinFloat32(v1.uv.x, MinFloat32(v2.uv.x, v3.uv.x)),
            MinFloat32(v1.uv.y, MinFloat32(v2.uv.y, v3.uv.y))
        };
        const Vec2 max = {
            MaxFloat32(v1.uv.x, MaxFloat32(v2.uv.x, v3.uv.x)),
            MaxFloat32(v1.uv.y, MaxFloat32(v2.uv.y, v3.uv.y))
        };
        const Vec2Int minPixel = { (int)(min.x * squareSize), (int)(min.y * squareSize) };
        const Vec2Int maxPixel = { (int)(max.x * squareSize), (int)(max.y * squareSize) };
        const float32 MARGIN_BARYCENTRIC = 0.0f;
        const float32 B_MIN = -MARGIN_BARYCENTRIC;
        const float32 B_MAX = 1.0f + MARGIN_BARYCENTRIC;

        for (int y = minPixel.y; y < maxPixel.y; y++) {
            for (int x = minPixel.x; x < maxPixel.x; x++) {
                const Vec2 uv = { (float32)x / squareSize, (float32)y / squareSize };
                const Vec3 bC = BarycentricCoordinates(uv, v1.uv, v2.uv, v3.uv);
                if (B_MIN <= bC.x && bC.x <= B_MAX && B_MIN <= bC.y && bC.y <= B_MAX && B_MIN <= bC.z && bC.z <= B_MAX) {
                    const Vec3 pos = v1.pos * bC.x + v2.pos * bC.y + v3.pos * bC.z;

                    uint8 r = (uint8)(pos.x * 255.0f);
                    uint8 g = (uint8)(pos.y * 255.0f);
                    uint8 b = (uint8)(pos.z * 255.0f);
                    uint8 a = 0xff;
                    pixels[y * squareSize + x] = (a << 24) + (b << 16) + (g << 8) + r;
                }
            }
        }
    }

#if 0
    for (int y = 0; y < squareSize; y++) {
        for (int x = 0; x < squareSize; x++) {
            Vec3 pixelPos = PixelToModelPosition(x, y, squareSize, model.vertices);

            uint8 r = 0xcc;
            uint8 g = 0xcc;
            uint8 b = 0xff;
            uint8 a = 0xff;

            r = (uint8)(pixelPos.x * 255.0f);
            g = (uint8)(pixelPos.y * 255.0f);
            b = (uint8)(pixelPos.z * 255.0f);
            pixels[y * squareSize + x] = (a << 24) + (b << 16) + (g << 8) + r;
        }
    }
#endif
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
        appState->cameraPos = Vec3 { -5.0f, 0.0f, 1.0f };
        appState->cameraAngles = Vec2 { 0.0f, 0.0f };

        memory->initialized = true;
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

APP_LOAD_VULKAN_STATE_FUNCTION(AppLoadVulkanState)
{
    const VulkanWindow& window = vulkanState.window;
    const VulkanSwapchain& swapchain = vulkanState.swapchain;

    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);
    LinearAllocator allocator(memory->transient);

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
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attributeDescriptions[3] = {};
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, normal);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, uv);

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

    LoadObjResult obj;
    if (!LoadObj(ToString("data/models/reference-scene.obj"), &obj, &allocator)) {
        LOG_ERROR("Failed to load reference scene .obj\n");
        return false;
    }
    uint32_t totalVertices = 0;
    for (uint64 i = 0; i < obj.models.size; i++) {
        totalVertices += (uint32_t)obj.models[i].vertices.size;
    }

    // Create texture image
    {
#if 0
        int width, height, channels;
        unsigned char* imageData = stbi_load("data/textures/texture.jpg", &width, &height, &channels, STBI_rgb_alpha);
        if (imageData == nullptr) {
            LOG_ERROR("Failed to load texture\n");
            return false;
        }
        defer(stbi_image_free(imageData));
#endif

        const int squareSize = 512;
        const VkDeviceSize imageSize = squareSize * squareSize * 4;
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        if (!CreateBuffer(imageSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          window.device, window.physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for staging buffer\n");
            return false;
        }
        defer({
                  vkDestroyBuffer(window.device, stagingBuffer, nullptr);
                  vkFreeMemory(window.device, stagingBufferMemory, nullptr);
              });

        void* data;
        vkMapMemory(window.device, stagingBufferMemory, 0, imageSize, 0, &data);

        CalculateLightmapForModel(obj.models, 3, squareSize, (uint32*)data);

        vkUnmapMemory(window.device, stagingBufferMemory);

        if (!CreateImage(window.device, window.physicalDevice, squareSize, squareSize,
                         VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         &app->textureImage, &app->textureImageMemory)) {
            LOG_ERROR("CreateImage failed\n");
            return false;
        }

        TransitionImageLayout(window.device, app->commandPool, window.graphicsQueue, app->textureImage,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyBufferToImage(window.device, app->commandPool, window.graphicsQueue,
                          stagingBuffer, app->textureImage, squareSize, squareSize);
        TransitionImageLayout(window.device, app->commandPool, window.graphicsQueue, app->textureImage,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    }

    // Create texture image view
    {
        if (!CreateImageView(window.device, app->textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                             VK_IMAGE_ASPECT_COLOR_BIT, &app->textureImageView)) {
            LOG_ERROR("CreateImageView failed\n");
            return false;
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

        if (vkCreateSampler(window.device, &createInfo, nullptr, &app->textureSampler) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSampler failed\n");
            return false;
        }
    }

    // Create vertex buffer
    // Depends on commandPool and graphicsQueue, which are created by swapchain,
    // but doesn't really need to be recreated with the swapchain
    {
        VkDeviceSize vertexBufferSize = totalVertices * sizeof(Vertex);

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
        uint64 offset = 0;
        for (uint64 i = 0; i < obj.models.size; i++) {
            const uint64 numBytes = obj.models[i].vertices.size * sizeof(Vertex);
            MemCopy((char*)data + offset, obj.models[i].vertices.data, numBytes);
            offset += numBytes;
        }
        vkUnmapMemory(window.device, stagingBufferMemory);

        // Copy vertex data from staging buffer into GPU vertex buffer
        CopyBuffer(window.device, app->commandPool, window.graphicsQueue,
                   stagingBuffer, app->vertexBuffer, vertexBufferSize);

        vkDestroyBuffer(window.device, stagingBuffer, nullptr);
        vkFreeMemory(window.device, stagingBufferMemory, nullptr);
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

    // Create descriptor pool
    {
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 1;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = C_ARRAY_LENGTH(poolSizes);
        poolInfo.pPoolSizes = poolSizes;
        poolInfo.maxSets = 1;

        if (vkCreateDescriptorPool(window.device, &poolInfo, nullptr, &app->descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorPool failed\n");
            return false;
        }
    }

    // Create descriptor set
    {
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = app->descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &app->descriptorSetLayout;

        if (vkAllocateDescriptorSets(window.device, &allocInfo, &app->descriptorSet) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateDescriptorSets failed\n");
            return false;
        }

        VkDescriptorBufferInfo bufferInfo = {};
        bufferInfo.buffer = app->uniformBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = app->textureImageView;
        imageInfo.sampler = app->textureSampler;

        VkWriteDescriptorSet descriptorWrites[2] = {};
        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = app->descriptorSet;
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pBufferInfo = &bufferInfo;

        descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[1].dstSet = app->descriptorSet;
        descriptorWrites[1].dstBinding = 1;
        descriptorWrites[1].dstArrayElement = 0;
        descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[1].descriptorCount = 1;
        descriptorWrites[1].pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(window.device, C_ARRAY_LENGTH(descriptorWrites), descriptorWrites, 0, nullptr);
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

        for (uint64 i = 0; i < app->commandBuffers.size; i++) {
            const VkCommandBuffer& buffer = app->commandBuffers[i];

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = 0;
            beginInfo.pInheritanceInfo = nullptr;

            if (vkBeginCommandBuffer(buffer, &beginInfo) != VK_SUCCESS) {
                LOG_ERROR("vkBeginCommandBuffer failed for command buffer %llu\n", i);
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

            vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, app->pipelineLayout, 0, 1,
                                    &app->descriptorSet, 0, nullptr);

            vkCmdDraw(buffer, totalVertices, 1, 0, 0);

            vkCmdEndRenderPass(buffer);

            if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
                LOG_ERROR("vkEndCommandBuffer failed for command buffer %llu\n", i);
                return false;
            }
        }
    }

    return true;
}

APP_UNLOAD_VULKAN_STATE_FUNCTION(AppUnloadVulkanState)
{
    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

    vkDestroyDescriptorPool(device, app->descriptorPool, nullptr);

    vkDestroyBuffer(device, app->uniformBuffer, nullptr);
    vkFreeMemory(device, app->uniformBufferMemory, nullptr);
    vkDestroyBuffer(device, app->vertexBuffer, nullptr);
    vkFreeMemory(device, app->vertexBufferMemory, nullptr);

    vkDestroySampler(device, app->textureSampler, nullptr);
    vkDestroyImageView(device, app->textureImageView, nullptr);
    vkDestroyImage(device, app->textureImage, nullptr);
    vkFreeMemory(device, app->textureImageMemory, nullptr);

    vkDestroyCommandPool(device, app->commandPool, nullptr);
    vkDestroyPipeline(device, app->graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(device, app->pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, app->descriptorSetLayout, nullptr);
}

#include "load_obj.cpp"
#include "vulkan.cpp"

#if GAME_WIN32
#include "win32_main.cpp"
#else
#error "Unsupported platform"
#endif

#include <km_common/km_array.cpp>
#include <km_common/km_container.cpp>
#include <km_common/km_input.cpp>
#include <km_common/km_memory.cpp>
#include <km_common/km_os.cpp>
#include <km_common/km_string.cpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
