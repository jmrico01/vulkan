#include <Windows.h>

#include <km_common/km_input.h>

#include "app_main.h"
#include "vulkan.h"

bool running_ = false;
bool windowSizeChange_ = false;
AppInput* input_ = nullptr;
global_var WINDOWPLACEMENT windowPlacementPrev_ = { sizeof(windowPlacementPrev_) };

// Application-specific Vulkan state
struct VulkanApp
{
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;

    VkCommandPool commandPool;
    FixedArray<VkCommandBuffer, VulkanSwapchain::MAX_IMAGES> commandBuffers;

    VkImage textureImage;
    VkDeviceMemory textureImageMemory;
    VkImageView textureImageView;
    VkSampler textureSampler;

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    VkBuffer uniformBuffer;
    VkDeviceMemory uniformBufferMemory;

    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSet;
};

bool LoadVulkanApp(const VulkanWindow& window, const VulkanSwapchain& swapchain, VulkanApp* app,
                   LinearAllocator* allocator)
{
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
        const Array<uint8> vertShaderCode = LoadEntireFile(ToString("data/shaders/shader.vert.spv"), allocator);
        if (vertShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load vertex shader code\n");
            return false;
        }
        const Array<uint8> fragShaderCode = LoadEntireFile(ToString("data/shaders/shader.frag.spv"), allocator);
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
        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(window.surface, window.physicalDevice, allocator);

        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.queueFamilyIndex = queueFamilyInfo.graphicsFamilyIndex;
        poolCreateInfo.flags = 0;

        if (vkCreateCommandPool(window.device, &poolCreateInfo, nullptr, &app->commandPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateCommandPool failed\n");
            return false;
        }
    }

    // Create texture image
    {
        int width, height, channels;
        unsigned char* imageData = stbi_load("data/textures/texture.jpg", &width, &height, &channels, STBI_rgb_alpha);
        if (imageData == nullptr) {
            LOG_ERROR("Failed to load texture\n");
            return false;
        }
        defer(stbi_image_free(imageData));

        const VkDeviceSize imageSize = width * height * 4;
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
        MemCopy(data, imageData, imageSize);
        vkUnmapMemory(window.device, stagingBufferMemory);

        if (!CreateImage(window.device, window.physicalDevice, width, height,
                         VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         &app->textureImage, &app->textureImageMemory)) {
            LOG_ERROR("CreateImage failed\n");
            return false;
        }

        TransitionImageLayout(window.device, app->commandPool, window.graphicsQueue, app->textureImage,
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyBufferToImage(window.device, app->commandPool, window.graphicsQueue,
                          stagingBuffer, app->textureImage, width, height);
        TransitionImageLayout(window.device, app->commandPool, window.graphicsQueue, app->textureImage,
                              VK_FORMAT_R8G8B8A8_SRGB,
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

    LoadObjResult obj;
    if (!LoadObj(ToString("data/models/reference-scene.obj"), &obj, allocator)) {
        LOG_ERROR("Failed to load reference scene .obj\n");
        return false;
    }
    uint32_t totalVertices = 0;
    for (uint64 i = 0; i < obj.models.size; i++) {
        totalVertices += (uint32_t)obj.models[i].vertices.size;
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

void UnloadVulkanApp(const VulkanWindow& window, VulkanApp* app)
{
    vkDestroyDescriptorPool(window.device, app->descriptorPool, nullptr);

    vkDestroyBuffer(window.device, app->uniformBuffer, nullptr);
    vkFreeMemory(window.device, app->uniformBufferMemory, nullptr);
    vkDestroyBuffer(window.device, app->vertexBuffer, nullptr);
    vkFreeMemory(window.device, app->vertexBufferMemory, nullptr);

    vkDestroySampler(window.device, app->textureSampler, nullptr);
    vkDestroyImageView(window.device, app->textureImageView, nullptr);
    vkDestroyImage(window.device, app->textureImage, nullptr);
    vkFreeMemory(window.device, app->textureImageMemory, nullptr);

    vkDestroyCommandPool(window.device, app->commandPool, nullptr);
    vkDestroyPipeline(window.device, app->graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(window.device, app->pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(window.device, app->descriptorSetLayout, nullptr);
}

KmKeyCode Win32KeyCodeToKm(int vkCode)
{
    // Numbers, letters, text
    if (vkCode >= 0x30 && vkCode <= 0x39) {
        return (KmKeyCode)(vkCode - 0x30 + KM_KEY_0);
    }
    else if (vkCode >= 0x41 && vkCode <= 0x5a) {
        return (KmKeyCode)(vkCode - 0x41 + KM_KEY_A);
    }
    else if (vkCode == VK_SPACE) {
        return KM_KEY_SPACE;
    }
    else if (vkCode == VK_BACK) {
        return KM_KEY_BACKSPACE;
    }
    // Arrow keys
    else if (vkCode == VK_UP) {
        return KM_KEY_ARROW_UP;
    }
    else if (vkCode == VK_DOWN) {
        return KM_KEY_ARROW_DOWN;
    }
    else if (vkCode == VK_LEFT) {
        return KM_KEY_ARROW_LEFT;
    }
    else if (vkCode == VK_RIGHT) {
        return KM_KEY_ARROW_RIGHT;
    }
    // Special keys
    else if (vkCode == VK_ESCAPE) {
        return KM_KEY_ESCAPE;
    }
    else if (vkCode == VK_SHIFT) {
        return KM_KEY_SHIFT;
    }
    else if (vkCode == VK_CONTROL) {
        return KM_KEY_CTRL;
    }
    else if (vkCode == VK_TAB) {
        return KM_KEY_TAB;
    }
    else if (vkCode == VK_RETURN) {
        return KM_KEY_ENTER;
    }
    else if (vkCode >= 0x60 && vkCode <= 0x69) {
        return (KmKeyCode)(vkCode - 0x60 + KM_KEY_NUMPAD_0);
    }
    else {
        return KM_KEY_INVALID;
    }
}

Vec2Int Win32GetRenderingViewportSize(HWND hWnd)
{
    RECT clientRect;
    GetClientRect(hWnd, &clientRect);
    return Vec2Int { clientRect.right - clientRect.left, clientRect.bottom - clientRect.top };
}

void Win32ToggleFullscreen(HWND hWnd)
{
    // This follows Raymond Chen's perscription for fullscreen toggling. See:
    // https://blogs.msdn.microsoft.com/oldnewthing/20100412-00/?p=14353

    DWORD dwStyle = GetWindowLong(hWnd, GWL_STYLE);
    if (dwStyle & WS_OVERLAPPEDWINDOW) {
        // Switch to fullscreen
        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
        HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY);
        if (GetWindowPlacement(hWnd, &windowPlacementPrev_) && GetMonitorInfo(hMonitor, &monitorInfo)) {
            SetWindowLong(hWnd, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hWnd, HWND_TOP,
                         monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                         monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                         monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }
    else {
        // Switch to windowed
        SetWindowLong(hWnd, GWL_STYLE, dwStyle | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hWnd, &windowPlacementPrev_);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        RECT clientRect;
        GetClientRect(hWnd, &clientRect);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    LRESULT result = 0;

    switch (message) {
        case WM_ACTIVATEAPP: {
            // TODO handle
        } break;
        case WM_CLOSE: {
            // TODO handle this with a message?
            running_ = false;
        } break;
        case WM_DESTROY: {
            // TODO handle this as an error?
            running_ = false;
        } break;

        case WM_SIZE: {
            // TODO is it ok to ignore this?d
            // windowSizeChange_ = true;
        } break;

        case WM_SYSKEYDOWN: {
            DEBUG_PANIC("WM_SYSKEYDOWN in WndProc");
        } break;
        case WM_SYSKEYUP: {
            DEBUG_PANIC("WM_SYSKEYUP in WndProc");
        } break;
        case WM_KEYDOWN: {
        } break;
        case WM_KEYUP: {
        } break;

        case WM_CHAR: {
            char c = (char)wParam;
            input_->keyboardString[input_->keyboardStringLen++] = c;
            input_->keyboardString[input_->keyboardStringLen] = '\0';
        } break;

        default: {
            result = DefWindowProc(hWnd, message, wParam, lParam);
        } break;
    }

    return result;
}

internal void Win32ProcessMessages(HWND hWnd, AppInput* input)
{
    MSG msg;
    while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
        switch (msg.message) {
            case WM_QUIT: {
                running_ = false;
            } break;

            case WM_SYSKEYDOWN: {
                uint32 vkCode = (uint32)msg.wParam;
                bool altDown = (msg.lParam & (1 << 29)) != 0;

                if (vkCode == VK_F4 && altDown) {
                    running_ = false;
                }
            } break;
            case WM_SYSKEYUP: {
            } break;

            case WM_KEYDOWN: {
                uint32 vkCode = (uint32)msg.wParam;
                bool wasDown = ((msg.lParam & (1 << 30)) != 0);
                bool isDown = ((msg.lParam & (1 << 31)) == 0);
                int transitions = (wasDown != isDown) ? 1 : 0;
                DEBUG_ASSERT(isDown);

                int kmKeyCode = Win32KeyCodeToKm(vkCode);
                if (kmKeyCode != KM_KEY_INVALID) {
                    input->keyboard[kmKeyCode].isDown = isDown;
                    input->keyboard[kmKeyCode].transitions = transitions;
                }

                if (vkCode == VK_F11) {
                    Win32ToggleFullscreen(hWnd);
                    windowSizeChange_ = true;
                }

                // Pass over to WndProc for WM_CHAR messages (string input)
                input_ = input;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } break;
            case WM_KEYUP: {
                uint32 vkCode = (uint32)msg.wParam;
                bool wasDown = ((msg.lParam & (1 << 30)) != 0);
                bool isDown = ((msg.lParam & (1 << 31)) == 0);
                int transitions = (wasDown != isDown) ? 1 : 0;
                DEBUG_ASSERT(!isDown);

                int kmKeyCode = Win32KeyCodeToKm(vkCode);
                if (kmKeyCode != KM_KEY_INVALID) {
                    input->keyboard[kmKeyCode].isDown = isDown;
                    input->keyboard[kmKeyCode].transitions = transitions;
                }
            } break;

            case WM_LBUTTONDOWN: {
                input->mouseButtons[KM_MOUSE_LEFT].isDown = true;
                input->mouseButtons[KM_MOUSE_LEFT].transitions = 1;
            } break;
            case WM_LBUTTONUP: {
                input->mouseButtons[KM_MOUSE_LEFT].isDown = false;
                input->mouseButtons[KM_MOUSE_LEFT].transitions = 1;
            } break;
            case WM_RBUTTONDOWN: {
                input->mouseButtons[KM_MOUSE_RIGHT].isDown = true;
                input->mouseButtons[KM_MOUSE_RIGHT].transitions = 1;
            } break;
            case WM_RBUTTONUP: {
                input->mouseButtons[KM_MOUSE_RIGHT].isDown = false;
                input->mouseButtons[KM_MOUSE_RIGHT].transitions = 1;
            } break;
            case WM_MBUTTONDOWN: {
                input->mouseButtons[KM_MOUSE_MIDDLE].isDown = true;
                input->mouseButtons[KM_MOUSE_MIDDLE].transitions = 1;
            } break;
            case WM_MBUTTONUP: {
                input->mouseButtons[KM_MOUSE_MIDDLE].isDown = false;
                input->mouseButtons[KM_MOUSE_MIDDLE].transitions = 1;
            } break;
            case WM_XBUTTONDOWN: {
                if ((msg.wParam & MK_XBUTTON1) != 0) {
                    input->mouseButtons[KM_MOUSE_ALT1].isDown = true;
                    input->mouseButtons[KM_MOUSE_ALT1].transitions = 1;
                }
                else if ((msg.wParam & MK_XBUTTON2) != 0) {
                    input->mouseButtons[KM_MOUSE_ALT2].isDown = true;
                    input->mouseButtons[KM_MOUSE_ALT2].transitions = 1;
                }
            } break;
            case WM_XBUTTONUP: {
                if ((msg.wParam & MK_XBUTTON1) != 0) {
                    input->mouseButtons[KM_MOUSE_ALT1].isDown = false;
                    input->mouseButtons[KM_MOUSE_ALT1].transitions = 1;
                }
                else if ((msg.wParam & MK_XBUTTON2) != 0) {
                    input->mouseButtons[KM_MOUSE_ALT2].isDown = false;
                    input->mouseButtons[KM_MOUSE_ALT2].transitions = 1;
                }
            } break;
            case WM_MOUSEWHEEL: {
                // TODO standardize these units
                input->mouseWheel += GET_WHEEL_DELTA_WPARAM(msg.wParam);
            } break;

            default: {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } break;
        }
    }
}

HWND Win32CreateWindow(HINSTANCE hInstance, WNDPROC wndProc, const char* className, const char* windowName,
                       int x, int y, int clientWidth, int clientHeight)
{
    WNDCLASSEX wndClass = { sizeof(wndClass) };
    wndClass.style = CS_HREDRAW | CS_VREDRAW;
    wndClass.lpfnWndProc = wndProc;
    wndClass.hInstance = hInstance;
    //wndClass.hIcon = NULL;
    wndClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndClass.lpszClassName = className;

    if (!RegisterClassEx(&wndClass)) {
        LOG_ERROR("RegisterClassEx call failed\n");
        return NULL;
    }

    RECT windowRect   = {};
    windowRect.left   = x;
    windowRect.top    = y;
    windowRect.right  = x + clientWidth;
    windowRect.bottom = y + clientHeight;

    if (!AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW | WS_VISIBLE, FALSE, 0)) {
        LOG_ERROR("AdjustWindowRectEx call failed\n");
        GetLastError();
        return NULL;
    }

    HWND hWindow = CreateWindowEx(0, className, windowName,
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  windowRect.left, windowRect.top,
                                  windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                                  0, 0, hInstance, 0);

    if (!hWindow) {
        LOG_ERROR("CreateWindowEx call failed\n");
        return NULL;
    }

    return hWindow;
}

int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    HWND hWnd = Win32CreateWindow(hInstance, WndProc, "VulkanWindowClass", "vulkan",
                                  100, 100, WINDOW_START_WIDTH, WINDOW_START_HEIGHT);
    if (!hWnd) {
        LOG_ERROR("Win32CreateWindow failed\n");
        LOG_FLUSH();
        return 1;
    }

#if GAME_INTERNAL
    LPVOID baseAddress = (LPVOID)TERABYTES(2);
#else
    LPVOID baseAddress = 0;
#endif

    // Initialize app memory
    Array<uint8> totalMemory;
    totalMemory.size = PERMANENT_MEMORY_SIZE + TRANSIENT_MEMORY_SIZE;
    totalMemory.data = (uint8*)VirtualAlloc(baseAddress, totalMemory.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!totalMemory.data) {
        LOG_ERROR("Win32 memory allocation failed\n");
        LOG_FLUSH();
        return 1;
    }
    AppMemory appMemory = {
        .initialized = false,
        .permanent = {
            .size = PERMANENT_MEMORY_SIZE,
            .data = totalMemory.data
        },
        .transient = {
            .size = TRANSIENT_MEMORY_SIZE,
            .data = totalMemory.data + PERMANENT_MEMORY_SIZE
        }
    };
    LOG_INFO("Initialized app memory, %llu bytes\n", totalMemory.size);

    // Initialize Vulkan
    Vec2Int windowSize = { WINDOW_START_WIDTH, WINDOW_START_HEIGHT };
    VulkanState vulkanState;
    VulkanApp vulkanApp;
    {
        LinearAllocator tempAllocator(appMemory.transient);
        if (!LoadVulkanState(&vulkanState, hInstance, hWnd, windowSize, &tempAllocator)) {
            LOG_ERROR("LoadVulkanState failed\n");
            LOG_FLUSH();
            return 1;
        }

        if (!LoadVulkanApp(vulkanState.window, vulkanState.swapchain, &vulkanApp, &tempAllocator)) {
            return false;
        }
    }
    LOG_INFO("Loaded Vulkan state, %llu swapchain images\n", vulkanState.swapchain.images.size);

    // Initialize audio
    AppAudio appAudio = {};

    AppInput input[2] = {};
    AppInput *newInput = &input[0];
    AppInput *oldInput = &input[1];

    // Initialize timing information
    int64 timerFreq;
    LARGE_INTEGER timerLast;
    uint64 cyclesLast;
    {
        LARGE_INTEGER timerFreqResult;
        QueryPerformanceFrequency(&timerFreqResult);
        timerFreq = timerFreqResult.QuadPart;

        QueryPerformanceCounter(&timerLast);
        cyclesLast = __rdtsc();
    }
    float32 lastElapsed = 0.0f;

    running_ = true;
    while (running_) {
        int mouseWheelPrev = newInput->mouseWheel;
        Win32ProcessMessages(hWnd, newInput);
        newInput->mouseWheelDelta = newInput->mouseWheel - mouseWheelPrev;

        if (windowSizeChange_) {
            windowSizeChange_ = false;
            // TODO duplicate from vkAcquireNextImageKHR out of date case
            Vec2Int newSize = Win32GetRenderingViewportSize(hWnd);
            LinearAllocator tempAllocator(appMemory.transient);

            vkDeviceWaitIdle(vulkanState.window.device);
            UnloadVulkanApp(vulkanState.window, &vulkanApp);
            if (!ReloadVulkanWindow(&vulkanState, hInstance, hWnd, newSize, &tempAllocator)) {
                DEBUG_PANIC("Failed to reload Vulkan window\n");
            }
            if (!LoadVulkanApp(vulkanState.window, vulkanState.swapchain, &vulkanApp, &tempAllocator)) {
                DEBUG_PANIC("Failed to reload Vulkan app\n");
            }
            continue;
        }

        const Vec2Int screenSize = {
            (int)vulkanState.swapchain.extent.width,
            (int)vulkanState.swapchain.extent.height
        };

        POINT mousePos;
        GetCursorPos(&mousePos);
        ScreenToClient(hWnd, &mousePos);
        Vec2Int mousePosPrev = newInput->mousePos;
        newInput->mousePos.x = mousePos.x;
        newInput->mousePos.y = screenSize.y - mousePos.y;
        newInput->mouseDelta = newInput->mousePos - mousePosPrev;
        if (mousePos.x < 0 || mousePos.x > screenSize.x || mousePos.y < 0 || mousePos.y > screenSize.y) {
            for (int i = 0; i < 5; i++) {
                int transitions = newInput->mouseButtons[i].isDown ? 1 : 0;
                newInput->mouseButtons[i].isDown = false;
                newInput->mouseButtons[i].transitions = transitions;
            }
        }

        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(vulkanState.window.device, vulkanState.swapchain.swapchain, UINT64_MAX,
                                                vulkanState.window.imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // TODO duplicate from windowSizeChange_
            Vec2Int newSize = Win32GetRenderingViewportSize(hWnd);
            LinearAllocator tempAllocator(appMemory.transient);

            vkDeviceWaitIdle(vulkanState.window.device);
            UnloadVulkanApp(vulkanState.window, &vulkanApp);
            if (!ReloadVulkanWindow(&vulkanState, hInstance, hWnd, newSize, &tempAllocator)) {
                DEBUG_PANIC("Failed to reload Vulkan window\n");
            }
            if (!LoadVulkanApp(vulkanState.window, vulkanState.swapchain, &vulkanApp, &tempAllocator)) {
                DEBUG_PANIC("Failed to reload Vulkan app\n");
            }
            continue;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            LOG_ERROR("Failed to acquire swapchain image\n");
        }

        static float32 totalElapsed = 0.0f;
        totalElapsed += lastElapsed;

        UniformBufferObject ubo;
        ubo.model = Mat4::one;

        static Vec3 cameraPos = { -5.0f, 0.0f, 1.0f };
        static Vec2 cameraAngles = { 0.0f, 0.0f };

        const float32 cameraSensitivity = 2.0f;
        if (MouseDown(*newInput, KM_MOUSE_LEFT)) {
            const Vec2 mouseDeltaFrac = {
                (float32)newInput->mouseDelta.x / (float32)screenSize.x,
                (float32)newInput->mouseDelta.y / (float32)screenSize.y
            };
            cameraAngles += mouseDeltaFrac * cameraSensitivity;

            cameraAngles.x = ModFloat32(cameraAngles.x, PI_F * 2.0f);
            cameraAngles.y = ClampFloat32(cameraAngles.y, -PI_F, PI_F);
        }

        const Quat cameraRotYaw = QuatFromAngleUnitAxis(cameraAngles.x, Vec3::unitZ);
        const Quat cameraRotPitch = QuatFromAngleUnitAxis(cameraAngles.y, Vec3::unitY);

        const Quat cameraRotYawInv = Inverse(cameraRotYaw);
        const Vec3 cameraForward = cameraRotYawInv * Vec3::unitX;
        const Vec3 cameraRight = cameraRotYawInv * -Vec3::unitY;
        const Vec3 cameraUp = Vec3::unitZ;

        const float32 speed = 2.0f;
        if (KeyDown(*newInput, KM_KEY_W)) {
            cameraPos += speed * cameraForward * lastElapsed;
        }
        if (KeyDown(*newInput, KM_KEY_S)) {
            cameraPos -= speed * cameraForward * lastElapsed;
        }
        if (KeyDown(*newInput, KM_KEY_A)) {
            cameraPos -= speed * cameraRight * lastElapsed;
        }
        if (KeyDown(*newInput, KM_KEY_D)) {
            cameraPos += speed * cameraRight * lastElapsed;
        }
        if (KeyDown(*newInput, KM_KEY_SPACE)) {
            cameraPos += speed * cameraUp * lastElapsed;
        }
        if (KeyDown(*newInput, KM_KEY_SHIFT)) {
            cameraPos -= speed * cameraUp * lastElapsed;
        }

        const Quat cameraRot = cameraRotPitch * cameraRotYaw;
        const Mat4 cameraRotMat4 = UnitQuatToMat4(cameraRot);

        // Transforms world-view camera (+X forward, +Z up) to Vulkan camera (+Z forward, -Y up)
        const Quat baseCameraRot = QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitY)
            * QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitX);
        const Mat4 baseCameraRotMat4 = UnitQuatToMat4(baseCameraRot);

        ubo.view = baseCameraRotMat4 * cameraRotMat4 * Translate(-cameraPos);

        const float32 aspect = (float32)screenSize.x / (float32)screenSize.y;
        const float32 nearZ = 0.1f;
        const float32 farZ = 50.0f;
        ubo.proj = Perspective(PI_F / 4.0f, aspect, nearZ, farZ);

        void* data;
        vkMapMemory(vulkanState.window.device, vulkanApp.uniformBufferMemory, 0, sizeof(ubo), 0, &data);
        MemCopy(data, &ubo, sizeof(ubo));
        vkUnmapMemory(vulkanState.window.device, vulkanApp.uniformBufferMemory);

        const VkSemaphore waitSemaphores[] = { vulkanState.window.imageAvailableSemaphore };
        const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

        const VkSemaphore signalSemaphores[] = { vulkanState.window.renderFinishedSemaphore };

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = C_ARRAY_LENGTH(waitSemaphores);
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &vulkanApp.commandBuffers[(uint64)imageIndex];
        submitInfo.signalSemaphoreCount = C_ARRAY_LENGTH(signalSemaphores);
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(vulkanState.window.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
            LOG_ERROR("Failed to submit draw command buffer\n");
        }

        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = C_ARRAY_LENGTH(signalSemaphores);
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &vulkanState.swapchain.swapchain;
        presentInfo.pImageIndices = &imageIndex;
        presentInfo.pResults = nullptr;

        vkQueuePresentKHR(vulkanState.window.presentQueue, &presentInfo);

        vkQueueWaitIdle(vulkanState.window.graphicsQueue);
        vkQueueWaitIdle(vulkanState.window.presentQueue);

        // timing information
        {
            LARGE_INTEGER timerEnd;
            QueryPerformanceCounter(&timerEnd);
            uint64 cyclesEnd = __rdtsc();

            int64 timerElapsed = timerEnd.QuadPart - timerLast.QuadPart;
            lastElapsed = (float32)timerElapsed / timerFreq;
            float32 elapsedMs = lastElapsed * 1000.0f;
            int64 cyclesElapsed = cyclesEnd - cyclesLast;
            float64 megaCyclesElapsed = (float64)cyclesElapsed / 1000000.0f;
            // LOG_INFO("elapsed %.03f ms | %.03f MC\n", elapsedMs, megaCyclesElapsed);

            timerLast = timerEnd;
            cyclesLast = cyclesEnd;
        }

        AppInput *temp = newInput;
        newInput = oldInput;
        oldInput = temp;
        ClearInput(newInput, *oldInput);
    }

    vkDeviceWaitIdle(vulkanState.window.device);
    UnloadVulkanApp(vulkanState.window, &vulkanApp);
    UnloadVulkanState(&vulkanState);

    return 0;
}
