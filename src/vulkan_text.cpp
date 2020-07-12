#include "vulkan_text.h"

void PushText(FontId fontId, const_string text, Vec2Int pos, float32 depth, Vec2Int screenSize,
              const VulkanTextPipeline& textPipeline, VulkanTextRenderState* renderState)
{
    const uint32 fontIndex = (uint32)fontId;
    const FontFace& fontFace = textPipeline.fontFaces[fontIndex];

    Vec2Int offset = Vec2Int::zero;
    int ind = 0;
    for (uint32 i = 0; i < text.size; i++) {
        const uint32 ch = text[i];
        const GlyphInfo& glyphInfo = fontFace.glyphInfo[ch];

        const Vec2Int glyphPos = pos + offset + glyphInfo.offset;
        const RectCoordsNdc ndc = ToRectCoordsNdc(glyphPos, glyphInfo.size, screenSize);

        VulkanTextInstanceData* instanceData = renderState->textInstanceData[fontIndex].Append();
        instanceData->pos = ToVec3(ndc.pos, depth);
        instanceData->size = ndc.size;
        instanceData->uvInfo = {
            glyphInfo.uvOrigin.x, glyphInfo.uvOrigin.y,
            glyphInfo.uvSize.x, glyphInfo.uvSize.y
        };

        offset += glyphInfo.advance / 64;
        ind++;
    }
}

void ResetTextRenderState(VulkanTextRenderState* renderState)
{
    for (uint32 i = 0; i < renderState->textInstanceData.SIZE; i++) {
        renderState->textInstanceData[i].Clear();
    }
}

void UploadAndSubmitTextDrawCommands(VkDevice device, VkCommandBuffer commandBuffer,
                                     const VulkanTextPipeline& textPipeline, const VulkanTextRenderState& renderState,
                                     LinearAllocator* allocator)
{
    Array<uint32> fontNumInstances = allocator->NewArray<uint32>(VulkanTextPipeline::MAX_FONTS);
    uint32 totalNumInstances = 0;
    for (uint32 i = 0; i < VulkanTextPipeline::MAX_FONTS; i++) {
        fontNumInstances[i] = renderState.textInstanceData[i].size;
        totalNumInstances += fontNumInstances[i];
    }

    if (totalNumInstances > VulkanTextPipeline::MAX_INSTANCES) {
        LOG_ERROR("Too many text instances: %lu, max %lu\n", totalNumInstances, VulkanTextPipeline::MAX_INSTANCES);
        // TODO what to do here?
        DEBUG_PANIC("too many text instances (chars)!\n");
    }

    Array<VulkanTextInstanceData> instanceData = allocator->NewArray<VulkanTextInstanceData>(totalNumInstances);
    uint32 instances = 0;
    for (uint32 i = 0; i < VulkanTextPipeline::MAX_FONTS; i++) {
        const uint32 numInstances = renderState.textInstanceData[i].size;
        const uint32 numBytes = numInstances * sizeof(VulkanTextInstanceData);
        MemCopy(instanceData.data + instances * sizeof(VulkanTextInstanceData),
                renderState.textInstanceData[i].data, numBytes);
        instances += numInstances;
    }

    if (instanceData.size > 0) {
        void* data;
        const uint32 bufferSize = instanceData.size * sizeof(VulkanTextInstanceData);
        vkMapMemory(device, textPipeline.instanceBufferMemory, 0, bufferSize, 0, &data);
        MemCopy(data, instanceData.data, bufferSize);
        vkUnmapMemory(device, textPipeline.instanceBufferMemory);
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipeline.pipeline);

    const VkBuffer vertexBuffers[] = { textPipeline.vertexBuffer, textPipeline.instanceBuffer };
    const VkDeviceSize offsets[] = { 0, 0 };
    vkCmdBindVertexBuffers(commandBuffer, 0, C_ARRAY_LENGTH(vertexBuffers), vertexBuffers, offsets);

    uint32 startInstance = 0;
    for (uint32 i = 0; i < fontNumInstances.size; i++) {
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipeline.pipelineLayout, 0, 1,
                                &textPipeline.descriptorSets[i], 0, nullptr);

        if (fontNumInstances[i] > 0) {
            vkCmdDraw(commandBuffer, 6, fontNumInstances[i], 0, startInstance);
            startInstance += fontNumInstances[i];
        }
    }
}

bool LoadTextPipelineSwapchain(const VulkanWindow& window, const VulkanSwapchain& swapchain, LinearAllocator* allocator,
                               VulkanTextPipeline* textPipeline)
{
    const Array<uint8> vertShaderCode = LoadEntireFile(ToString("data/shaders/text.vert.spv"), allocator);
    if (vertShaderCode.data == nullptr) {
        LOG_ERROR("Failed to load vertex shader code\n");
        return false;
    }
    const Array<uint8> fragShaderCode = LoadEntireFile(ToString("data/shaders/text.frag.spv"), allocator);
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
    pipelineLayoutCreateInfo.pSetLayouts = &textPipeline->descriptorSetLayout;
    pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
    pipelineLayoutCreateInfo.pPushConstantRanges = nullptr;

    if (vkCreatePipelineLayout(window.device, &pipelineLayoutCreateInfo, nullptr,
                               &textPipeline->pipelineLayout) != VK_SUCCESS) {
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
    pipelineCreateInfo.layout = textPipeline->pipelineLayout;
    pipelineCreateInfo.renderPass = swapchain.renderPass;
    pipelineCreateInfo.subpass = 0;
    pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineCreateInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(window.device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr,
                                  &textPipeline->pipeline) != VK_SUCCESS) {
        LOG_ERROR("vkCreateGraphicsPipeline failed\n");
        return false;
    }

    return true;
}

void UnloadTextPipelineSwapchain(VkDevice device, VulkanTextPipeline* textPipeline)
{
    vkDestroyPipeline(device, textPipeline->pipeline, nullptr);
    vkDestroyPipelineLayout(device, textPipeline->pipelineLayout, nullptr);
}

bool LoadTextPipelineWindow(const VulkanWindow& window, VkCommandPool commandPool, LinearAllocator* allocator,
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
        const VkDeviceSize bufferSize = VulkanTextPipeline::MAX_INSTANCES * sizeof(VulkanTextInstanceData);

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

void UnloadTextPipelineWindow(VkDevice device, VulkanTextPipeline* textPipeline)
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
