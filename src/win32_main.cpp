#include <Windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <km_common/km_container.h>
#include <km_common/km_lib.h>
#include <km_common/km_math.h>
#include <km_common/km_os.h>
#include <km_common/km_string.h>

bool running_ = false;

struct VulkanState
{
    static const uint32 MAX_SWAPCHAIN_IMAGES = 16;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkSurfaceKHR surface;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;

    VkSwapchainKHR swapchain;
    VkFormat swapchainImageFormat;
    VkExtent2D swapchainExtent;

    FixedArray<VkImage, MAX_SWAPCHAIN_IMAGES> swapchainImages;
    FixedArray<VkImageView, MAX_SWAPCHAIN_IMAGES> swapchainImageViews;

    VkRenderPass renderPass;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;
    FixedArray<VkFramebuffer, MAX_SWAPCHAIN_IMAGES> swapchainFramebuffers;

    VkCommandPool commandPool;
    FixedArray<VkCommandBuffer, MAX_SWAPCHAIN_IMAGES> commandBuffers;

    VkSemaphore imageAvailableSemaphore;
    VkSemaphore renderFinishedSemaphore;
};

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                          VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                          const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                          void* pUserData)
{
    LOG_ERROR("Validation layer, message (severity %d, type %d): %s\n",
              messageSeverity, messageType, pCallbackData->pMessage);
    return VK_FALSE;
}

struct QueueFamilyInfo
{
    bool hasGraphicsFamily;
    uint32_t graphicsFamilyIndex;
    bool hasPresentFamily;
    uint32_t presentFamilyIndex;
};

QueueFamilyInfo GetQueueFamilyInfo(VkSurfaceKHR surface, VkPhysicalDevice device, LinearAllocator* allocator)
{
    QueueFamilyInfo info;
    info.hasGraphicsFamily = false;
    info.hasPresentFamily = false;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    DynamicArray<VkQueueFamilyProperties, LinearAllocator> queueFamilies(queueFamilyCount, allocator);
    queueFamilies.size = queueFamilyCount;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data);

    for (uint64 i = 0; i < queueFamilies.size; i++) {
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, (uint32_t)i, surface, &presentSupport);
        if (presentSupport) {
            info.hasPresentFamily = true;
            info.presentFamilyIndex = (uint32_t)i;
        }

        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            info.hasGraphicsFamily = true;
            info.graphicsFamilyIndex = (uint32_t)i;
        }
    }

    return info;
}

struct SwapchainSupportInfo
{
    VkSurfaceCapabilitiesKHR capabilities;
    DynamicArray<VkSurfaceFormatKHR, LinearAllocator> formats;
    DynamicArray<VkPresentModeKHR, LinearAllocator> presentModes;
};

void GetSwapchainSupportInfo(VkSurfaceKHR surface, VkPhysicalDevice device, SwapchainSupportInfo* supportInfo)
{
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &supportInfo->capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    if (formatCount > 0) {
        supportInfo->formats.UpdateCapacity(formatCount);
        supportInfo->formats.size = formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, supportInfo->formats.data);
    }

    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    if (presentModeCount > 0) {
        supportInfo->presentModes.UpdateCapacity(presentModeCount);
        supportInfo->presentModes.size = presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, supportInfo->presentModes.data);
    }
}

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const Array<VkSurfaceFormatKHR>& availableFormats)
{
    for (uint64 i = 0; i < availableFormats.size; i++) {
        if (availableFormats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormats[i];
        }
    }

    return availableFormats[0]; // sloppy fallback
}

VkPresentModeKHR ChooseSwapPresentMode(const Array<VkPresentModeKHR>& availablePresentModes)
{
    // NOTE VK_PRESENT_MODE_FIFO_KHR is guaranteed to be available, so we just return that
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, Vec2Int screenSize)
{
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    else {
        VkExtent2D actualExtent = {
            .width = (uint32_t)screenSize.x,
            .height = (uint32_t)screenSize.y
        };

        actualExtent.width = ClampInt(actualExtent.width,
                                      capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = ClampInt(actualExtent.height,
                                       capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return actualExtent;
    }
}

bool IsPhysicalDeviceSuitable(VkSurfaceKHR surface, VkPhysicalDevice device, const Array<const char*> requiredExtensions,
                              LinearAllocator* allocator)
{
    QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(surface, device, allocator);
    if (!queueFamilyInfo.hasGraphicsFamily || !queueFamilyInfo.hasPresentFamily) {
        LOG_ERROR("Surface and device missing graphics or present families\n");
        return false;
    }

    uint32_t extensionCount;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
        LOG_ERROR("vkEnumerateDeviceExtensionProperties failed\n");
        return false;
    }

    DynamicArray<VkExtensionProperties, LinearAllocator> extensions(extensionCount, allocator);
    extensions.size = extensionCount;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data) != VK_SUCCESS) {
        LOG_ERROR("vkEnumerateDeviceExtensionProperties failed\n");
        return false;
    }

    for (uint64 i = 0; i < requiredExtensions.size; i++) {
        const_string requiredExtension = ToString(requiredExtensions[i]);

        bool found = false;
        for (uint64 j = 0; j < extensions.size; j++) {
            const_string extensionName = ToString(extensions[j].extensionName);
            if (StringEquals(requiredExtension, extensionName)) {
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_ERROR("Required Vulkan device extension not found: %.*s\n",
                      (int)requiredExtension.size, requiredExtension.data);
            return false;
        }
    }

    SwapchainSupportInfo swapchainSupport;
    GetSwapchainSupportInfo(surface, device, &swapchainSupport);
    if (swapchainSupport.formats.size == 0 || swapchainSupport.presentModes.size == 0) {
        LOG_ERROR("Insufficient swap chain capabilities (%llu formats, %llu presentModes)\n",
                  swapchainSupport.formats.size, swapchainSupport.presentModes.size);
        return false;
    }

    return true;
}

bool CreateShaderModule(const Array<uint8> code, VkDevice device, VkShaderModule* shaderModule)
{
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size;
    createInfo.pCode = (const uint32_t*)code.data;

    return vkCreateShaderModule(device, &createInfo, nullptr, shaderModule) == VK_SUCCESS;
}

bool RecreateVulkanSwapchain(VulkanState* state, Vec2Int size, LinearAllocator* allocator)
{
    // Create swapchain
    {
        SwapchainSupportInfo swapchainSupportInfo;
        GetSwapchainSupportInfo(state->surface, state->physicalDevice, &swapchainSupportInfo);

        const VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapchainSupportInfo.formats.ToArray());
        const VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapchainSupportInfo.presentModes.ToArray());
        const VkExtent2D extent = ChooseSwapExtent(swapchainSupportInfo.capabilities, size);

        uint32_t imageCount = swapchainSupportInfo.capabilities.minImageCount + 1;
        if (swapchainSupportInfo.capabilities.maxImageCount > 0 &&
            imageCount > swapchainSupportInfo.capabilities.maxImageCount) {
            imageCount = swapchainSupportInfo.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = state->surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(state->surface, state->physicalDevice, allocator);
        uint32_t queueFamilyIndices[] = {
            queueFamilyInfo.graphicsFamilyIndex,
            queueFamilyInfo.presentFamilyIndex
        };
        if (queueFamilyInfo.graphicsFamilyIndex != queueFamilyInfo.presentFamilyIndex) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = swapchainSupportInfo.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(state->device, &createInfo, nullptr, &state->swapchain) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSwapchainKHR failed\n");
            return false;
        }

        state->swapchainImageFormat = surfaceFormat.format;
        state->swapchainExtent = extent;
        vkGetSwapchainImagesKHR(state->device, state->swapchain, &imageCount, nullptr);
        if (imageCount > VulkanState::MAX_SWAPCHAIN_IMAGES) {
            LOG_ERROR("Too many swapchain images: %lu\n", imageCount);
            return false;
        }
        state->swapchainImages.size = imageCount;
        vkGetSwapchainImagesKHR(state->device, state->swapchain, &imageCount, state->swapchainImages.data);

        state->swapchainImageViews.size = imageCount;
        for (uint64 i = 0; i < state->swapchainImages.size; i++) {
            VkImageViewCreateInfo imageViewCreateInfo = {};
            imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            imageViewCreateInfo.image = state->swapchainImages[i];
            imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            imageViewCreateInfo.format = surfaceFormat.format;
            imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
            imageViewCreateInfo.subresourceRange.levelCount = 1;
            imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
            imageViewCreateInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(state->device, &imageViewCreateInfo, nullptr,
                                  &state->swapchainImageViews[i]) != VK_SUCCESS) {
                LOG_ERROR("vkCreateImageView failed for image %llu\n", i);
                return false;
            }
        }
    }

    // Create render pass
    {
        VkAttachmentDescription colorAttachment = {};
        colorAttachment.format = state->swapchainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentRef = {};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassCreateInfo = {};
        renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.attachmentCount = 1;
        renderPassCreateInfo.pAttachments = &colorAttachment;
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpass;
        renderPassCreateInfo.dependencyCount = 1;
        renderPassCreateInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(state->device, &renderPassCreateInfo, nullptr, &state->renderPass) != VK_SUCCESS) {
            LOG_ERROR("vkCreateRenderPass failed\n");
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
        if (!CreateShaderModule(vertShaderCode, state->device, &vertShaderModule)) {
            LOG_ERROR("Failed to create vertex shader module\n");
            return false;
        }
        defer(vkDestroyShaderModule(state->device, vertShaderModule, nullptr));

        VkShaderModule fragShaderModule;
        if (!CreateShaderModule(fragShaderCode, state->device, &fragShaderModule)) {
            LOG_ERROR("Failed to create fragment shader module\n");
            return false;
        }
        defer(vkDestroyShaderModule(state->device, fragShaderModule, nullptr));

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

        VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo = {};
        vertexInputCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputCreateInfo.vertexBindingDescriptionCount = 0;
        vertexInputCreateInfo.pVertexBindingDescriptions = nullptr;
        vertexInputCreateInfo.vertexAttributeDescriptionCount = 0;
        vertexInputCreateInfo.pVertexAttributeDescriptions = nullptr;

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyCreateInfo = {};
        inputAssemblyCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyCreateInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssemblyCreateInfo.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float32)state->swapchainExtent.width;
        viewport.height = (float32)state->swapchainExtent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = state->swapchainExtent;

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
        pipelineLayoutCreateInfo.setLayoutCount = 0;
        pipelineLayoutCreateInfo.pSetLayouts = nullptr;
        pipelineLayoutCreateInfo.pushConstantRangeCount = 0;
        pipelineLayoutCreateInfo.pPushConstantRanges = nullptr;

        if (vkCreatePipelineLayout(state->device, &pipelineLayoutCreateInfo, nullptr, &state->pipelineLayout) != VK_SUCCESS) {
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
        pipelineCreateInfo.pDepthStencilState = nullptr;
        pipelineCreateInfo.pColorBlendState = &colorBlendingCreateInfo;
        pipelineCreateInfo.pDynamicState = nullptr;
        pipelineCreateInfo.layout = state->pipelineLayout;
        pipelineCreateInfo.renderPass = state->renderPass;
        pipelineCreateInfo.subpass = 0;
        pipelineCreateInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineCreateInfo.basePipelineIndex = -1;

        if (vkCreateGraphicsPipelines(state->device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &state->graphicsPipeline) != VK_SUCCESS) {
            LOG_ERROR("vkCreateGraphicsPipeline failed\n");
            return false;
        }
    }

    // Create framebuffers
    {
        state->swapchainFramebuffers.size = state->swapchainImageViews.size;
        for (uint64 i = 0; i < state->swapchainFramebuffers.size; i++) {
            VkImageView attachments[] = {
                state->swapchainImageViews[i]
            };

            VkFramebufferCreateInfo framebufferCreateInfo = {};
            framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferCreateInfo.renderPass = state->renderPass;
            framebufferCreateInfo.attachmentCount = C_ARRAY_LENGTH(attachments);
            framebufferCreateInfo.pAttachments = attachments;
            framebufferCreateInfo.width = state->swapchainExtent.width;
            framebufferCreateInfo.height = state->swapchainExtent.height;
            framebufferCreateInfo.layers = 1;

            if (vkCreateFramebuffer(state->device, &framebufferCreateInfo, nullptr,
                                    &state->swapchainFramebuffers[i]) != VK_SUCCESS) {
                LOG_ERROR("vkCreateFramebuffer failed for framebuffer %llu\n", i);
                return false;
            }
        }
    }

    // Create command pool
    {
        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(state->surface, state->physicalDevice, allocator);

        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.queueFamilyIndex = queueFamilyInfo.graphicsFamilyIndex;
        poolCreateInfo.flags = 0;

        if (vkCreateCommandPool(state->device, &poolCreateInfo, nullptr, &state->commandPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateCommandPool failed\n");
            return false;
        }
    }

    // Create command buffers
    {
        state->commandBuffers.size = state->swapchainFramebuffers.size;

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = state->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)state->commandBuffers.size;

        if (vkAllocateCommandBuffers(state->device, &allocInfo, state->commandBuffers.data) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateCommandBuffers failed\n");
            return false;
        }

        for (uint64 i = 0; i < state->commandBuffers.size; i++) {
            const VkCommandBuffer& buffer = state->commandBuffers[i];

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = 0;
            beginInfo.pInheritanceInfo = nullptr;

            if (vkBeginCommandBuffer(buffer, &beginInfo) != VK_SUCCESS) {
                LOG_ERROR("vkBeginCommandBuffer failed for command buffer %llu\n", i);
                return false;
            }

            const VkClearValue clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };

            VkRenderPassBeginInfo renderPassInfo = {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = state->renderPass;
            renderPassInfo.framebuffer = state->swapchainFramebuffers[i];
            renderPassInfo.renderArea.offset = { 0, 0 };
            renderPassInfo.renderArea.extent = state->swapchainExtent;
            renderPassInfo.clearValueCount = 1;
            renderPassInfo.pClearValues = &clearColor;

            vkCmdBeginRenderPass(buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state->graphicsPipeline);
            vkCmdDraw(buffer, 3, 1, 0, 0);

            vkCmdEndRenderPass(buffer);

            if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
                LOG_ERROR("vkEndCommandBuffer failed for command buffer %llu\n", i);
                return false;
            }
        }
    }

    return true;
}

bool LoadVulkanState(VulkanState* state, HINSTANCE hInstance, HWND hWnd, Vec2Int size, LinearAllocator* allocator)
{
    // Verify required layers
    const char* requiredLayers[] = {
        "VK_LAYER_KHRONOS_validation",
    };
    {
        uint32_t count;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
            LOG_ERROR("vkEnumerateInstanceLayerProperties failed\n");
            return false;
        }

        DynamicArray<VkLayerProperties, LinearAllocator> layers(count, allocator);
        layers.size = count;
        if (vkEnumerateInstanceLayerProperties(&count, layers.data) != VK_SUCCESS) {
            LOG_ERROR("vkEnumerateInstanceLayerProperties failed\n");
            return false;
        }

        for (int i = 0; i < C_ARRAY_LENGTH(requiredLayers); i++) {
            const_string requiredLayer = ToString(requiredLayers[i]);

            bool found = false;
            for (uint64 j = 0; j < layers.size; j++) {
                const_string layerName = ToString(layers[j].layerName);
                if (StringEquals(requiredLayer, layerName)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                LOG_ERROR("Required Vulkan instance layer not found: %.*s\n", (int)requiredLayer.size, requiredLayer.data);
                return false;
            }
        }
    }

    // Verify required extensions
    const char* requiredExtensions[] = {
        "VK_KHR_surface",
        "VK_KHR_win32_surface",
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };
    {
        uint32_t count;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
            LOG_ERROR("vkEnumerateInstanceExtensionProperties failed\n");
            return false;
        }

        DynamicArray<VkExtensionProperties, LinearAllocator> extensions(count, allocator);
        extensions.size = count;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data) != VK_SUCCESS) {
            LOG_ERROR("vkEnumerateInstanceExtensionProperties failed\n");
            return false;
        }

        for (int i = 0; i < C_ARRAY_LENGTH(requiredExtensions); i++) {
            const_string requiredExtension = ToString(requiredExtensions[i]);

            bool found = false;
            for (uint64 j = 0; j < extensions.size; j++) {
                const_string extensionName = ToString(extensions[j].extensionName);
                if (StringEquals(requiredExtension, extensionName)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                LOG_ERROR("Required Vulkan instance extension not found: %.*s\n",
                          (int)requiredExtension.size, requiredExtension.data);
            }
        }
    }

    // Create VkInstance
    {
        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "vulkan";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "km3d";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        // TODO don't do this in release mode
        createInfo.enabledLayerCount = C_ARRAY_LENGTH(requiredLayers);
        createInfo.ppEnabledLayerNames = requiredLayers;
        createInfo.enabledExtensionCount = C_ARRAY_LENGTH(requiredExtensions);
        createInfo.ppEnabledExtensionNames = requiredExtensions;

        if (vkCreateInstance(&createInfo, nullptr, &state->instance) != VK_SUCCESS) {
            LOG_ERROR("vkCreateInstance failed\n");
            return false;
        }
    }

    // Set up debug messenger
    {
        VkDebugUtilsMessengerCreateInfoEXT createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT; // For general debug info, add VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = VulkanDebugCallback;
        createInfo.pUserData = nullptr;

        auto vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(state->instance, "vkCreateDebugUtilsMessengerEXT");
        if (vkCreateDebugUtilsMessengerEXT == nullptr) {
            LOG_ERROR("vkGetInstanceProcAddr failed for vkCreateDebugUtilsMessengerEXT\n");
            return false;
        }

        if (vkCreateDebugUtilsMessengerEXT(state->instance, &createInfo, nullptr, &state->debugMessenger) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDebugUtilsMessengerEXT failed\n");
            return false;
        }
    }

    // Create window surface
    {
        VkWin32SurfaceCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hwnd = hWnd;
        createInfo.hinstance = hInstance;

        if (vkCreateWin32SurfaceKHR(state->instance, &createInfo, nullptr, &state->surface) != VK_SUCCESS) {
            LOG_ERROR("vkCreateWin32SurfaceKHR failed\n");
            return false;
        }
    }

    // Select physical device
    {
        const char* requiredDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };
        Array<const char*> requiredDeviceExtensionsArray = {
            .size = C_ARRAY_LENGTH(requiredDeviceExtensions),
            .data = requiredDeviceExtensions
        };

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(state->instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            LOG_ERROR("vkEnumeratePhysicalDevices returned 0 devices - no GPUs with Vulkan support\n");
            return false;
        }

        DynamicArray<VkPhysicalDevice, LinearAllocator> devices(deviceCount, allocator);
        devices.size = deviceCount;
        vkEnumeratePhysicalDevices(state->instance, &deviceCount, devices.data);

        state->physicalDevice = VK_NULL_HANDLE;
        for (uint64 i = 0; i < devices.size; i++) {
            if (IsPhysicalDeviceSuitable(state->surface, devices[i], requiredDeviceExtensionsArray, allocator)) {
                state->physicalDevice = devices[i];
                break;
            }
        }

        if (state->physicalDevice == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to find a suitable GPU for Vulkan\n");
            return false;
        }

        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(state->surface, state->physicalDevice, allocator);
        uint32_t queueFamilyIndices[] = {
            queueFamilyInfo.graphicsFamilyIndex,
            queueFamilyInfo.presentFamilyIndex
        };

        DynamicArray<VkDeviceQueueCreateInfo, LinearAllocator> queueCreateInfos(allocator);
        float32 queuePriority = 1.0f;
        for (int i = 0; i < C_ARRAY_LENGTH(queueFamilyIndices); i++) {
            bool repeatIndex = false;
            for (int j = 0; j < i; j++) {
                if (queueFamilyIndices[i] == queueFamilyIndices[j]) {
                    repeatIndex = true;
                    break;
                }
            }
            if (repeatIndex) {
                continue;
            }

            VkDeviceQueueCreateInfo* queueCreateInfo = queueCreateInfos.Append();
            queueCreateInfo->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo->queueFamilyIndex = queueFamilyIndices[i];
            queueCreateInfo->queueCount = 1;
            queueCreateInfo->pQueuePriorities = &queuePriority;
        }

        VkPhysicalDeviceFeatures deviceFeatures = {};

        VkDeviceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos = queueCreateInfos.data;
        createInfo.queueCreateInfoCount = (uint32_t)queueCreateInfos.size;
        createInfo.pEnabledFeatures = &deviceFeatures;

        createInfo.enabledExtensionCount = (uint32_t)requiredDeviceExtensionsArray.size;
        createInfo.ppEnabledExtensionNames = requiredDeviceExtensionsArray.data;
        // TODO don't do this in release mode
        createInfo.enabledLayerCount = C_ARRAY_LENGTH(requiredLayers);
        createInfo.ppEnabledLayerNames = requiredLayers;

        if (vkCreateDevice(state->physicalDevice, &createInfo, nullptr, &state->device) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDevice failed\n");
            return false;
        }

        vkGetDeviceQueue(state->device, queueFamilyInfo.graphicsFamilyIndex, 0, &state->graphicsQueue);
        vkGetDeviceQueue(state->device, queueFamilyInfo.presentFamilyIndex, 0, &state->presentQueue);
    }

    // Create semaphores
    {
        VkSemaphoreCreateInfo semaphoreCreateInfo = {};
        semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        if (vkCreateSemaphore(state->device, &semaphoreCreateInfo, nullptr,
                              &state->imageAvailableSemaphore) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSemaphore failed\n");
            return false;
        }
        if (vkCreateSemaphore(state->device, &semaphoreCreateInfo, nullptr,
                              &state->renderFinishedSemaphore) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSemaphore failed\n");
            return false;
        }
    }

    return RecreateVulkanSwapchain(state, size, allocator);
}

void UnloadVulkanSwapchain(VulkanState* state)
{
    vkDestroyCommandPool(state->device, state->commandPool, nullptr);
    for (uint64 i = 0; i < state->swapchainFramebuffers.size; i++) {
        vkDestroyFramebuffer(state->device, state->swapchainFramebuffers[i], nullptr);
    }
    vkDestroyPipeline(state->device, state->graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(state->device, state->pipelineLayout, nullptr);
    vkDestroyRenderPass(state->device, state->renderPass, nullptr);
    for (uint64 i = 0; i < state->swapchainImageViews.size; i++) {
        vkDestroyImageView(state->device, state->swapchainImageViews[i], nullptr);
    }
    vkDestroySwapchainKHR(state->device, state->swapchain, nullptr);
}

void UnloadVulkanState(VulkanState* state)
{
    UnloadVulkanSwapchain(state);

    auto vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(state->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (vkDestroyDebugUtilsMessengerEXT == nullptr) {
        LOG_ERROR("vkGetInstanceProcAddr failed for vkDestroyDebugUtilsMessengerEXT\n");
    }

    vkDestroySemaphore(state->device, state->renderFinishedSemaphore, nullptr);
    vkDestroySemaphore(state->device, state->imageAvailableSemaphore, nullptr);
    vkDestroyDevice(state->device, nullptr);
    vkDestroySurfaceKHR(state->instance, state->surface, nullptr);
    vkDestroyDebugUtilsMessengerEXT(state->instance, state->debugMessenger, nullptr);
    vkDestroyInstance(state->instance, nullptr);
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

#if 0
        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            if (glViewport_) {
                glViewport_(0, 0, width, height);
            }
            if (screenInfo_) {
                screenInfo_->size.x = width;
                screenInfo_->size.y = height;
                screenInfo_->changed = true;
            }
        } break;

        case WM_SYSKEYDOWN: {
            // DEBUG_PANIC("WM_SYSKEYDOWN in WndProc");
        } break;
        case WM_SYSKEYUP: {
            // DEBUG_PANIC("WM_SYSKEYUP in WndProc");
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
#endif

        default: {
            result = DefWindowProc(hWnd, message, wParam, lParam);
        } break;
    }

    return result;
}

internal void Win32ProcessMessages(HWND hWnd)
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
#if 0
            case WM_KEYDOWN: {
                uint32 vkCode = (uint32)msg.wParam;
                bool wasDown = ((msg.lParam & (1 << 30)) != 0);
                bool isDown = ((msg.lParam & (1 << 31)) == 0);
                int transitions = (wasDown != isDown) ? 1 : 0;
                DEBUG_ASSERT(isDown);

                int kmKeyCode = Win32KeyCodeToKM(vkCode);
                if (kmKeyCode != -1) {
                    gameInput->keyboard[kmKeyCode].isDown = isDown;
                    gameInput->keyboard[kmKeyCode].transitions = transitions;
                }

                if (vkCode == VK_F11) {
                    Win32ToggleFullscreen(hWnd, glFuncs);
                }

                // Pass over to WndProc for WM_CHAR messages (string input)
                input_ = gameInput;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } break;
            case WM_KEYUP: {
                uint32 vkCode = (uint32)msg.wParam;
                bool wasDown = ((msg.lParam & (1 << 30)) != 0);
                bool isDown = ((msg.lParam & (1 << 31)) == 0);
                int transitions = (wasDown != isDown) ? 1 : 0;
                DEBUG_ASSERT(!isDown);

                int kmKeyCode = Win32KeyCodeToKM(vkCode);
                if (kmKeyCode != -1) {
                    gameInput->keyboard[kmKeyCode].isDown = isDown;
                    gameInput->keyboard[kmKeyCode].transitions = transitions;
                }
            } break;

            case WM_LBUTTONDOWN: {
                gameInput->mouseButtons[0].isDown = true;
                gameInput->mouseButtons[0].transitions = 1;
            } break;
            case WM_LBUTTONUP: {
                gameInput->mouseButtons[0].isDown = false;
                gameInput->mouseButtons[0].transitions = 1;
            } break;
            case WM_RBUTTONDOWN: {
                gameInput->mouseButtons[1].isDown = true;
                gameInput->mouseButtons[1].transitions = 1;
            } break;
            case WM_RBUTTONUP: {
                gameInput->mouseButtons[1].isDown = false;
                gameInput->mouseButtons[1].transitions = 1;
            } break;
            case WM_MBUTTONDOWN: {
                gameInput->mouseButtons[2].isDown = true;
                gameInput->mouseButtons[2].transitions = 1;
            } break;
            case WM_MBUTTONUP: {
                gameInput->mouseButtons[2].isDown = false;
                gameInput->mouseButtons[2].transitions = 1;
            } break;
            case WM_XBUTTONDOWN: {
                if ((msg.wParam & MK_XBUTTON1) != 0) {
                    gameInput->mouseButtons[3].isDown = true;
                    gameInput->mouseButtons[3].transitions = 1;
                }
                else if ((msg.wParam & MK_XBUTTON2) != 0) {
                    gameInput->mouseButtons[4].isDown = true;
                    gameInput->mouseButtons[4].transitions = 1;
                }
            } break;
            case WM_XBUTTONUP: {
                if ((msg.wParam & MK_XBUTTON1) != 0) {
                    gameInput->mouseButtons[3].isDown = false;
                    gameInput->mouseButtons[3].transitions = 1;
                }
                else if ((msg.wParam & MK_XBUTTON2) != 0) {
                    gameInput->mouseButtons[4].isDown = false;
                    gameInput->mouseButtons[4].transitions = 1;
                }
            } break;
            case WM_MOUSEWHEEL: {
                // TODO standardize these units
                gameInput->mouseWheel += GET_WHEEL_DELTA_WPARAM(msg.wParam);
            } break;
#endif
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

    Array<uint8> totalMemory;
    totalMemory.size = PERMANENT_MEMORY_SIZE + TRANSIENT_MEMORY_SIZE;
    totalMemory.data = (uint8*)VirtualAlloc(baseAddress, totalMemory.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!totalMemory.data) {
        LOG_ERROR("Win32 memory allocation failed\n");
        LOG_FLUSH();
        return 1;
    }

    const Array<uint8> permanentMemory = {
        .size = PERMANENT_MEMORY_SIZE,
        .data = totalMemory.data
    };
    const Array<uint8> transientMemory = {
        .size = TRANSIENT_MEMORY_SIZE,
        .data = totalMemory.data + PERMANENT_MEMORY_SIZE
    };

    Vec2Int windowSize = { WINDOW_START_WIDTH, WINDOW_START_HEIGHT };
    VulkanState vulkanState;
    {
        LinearAllocator tempAllocator(transientMemory);
        if (!LoadVulkanState(&vulkanState, hInstance, hWnd, windowSize, &tempAllocator)) {
            LOG_ERROR("LoadVulkanState failed\n");
            LOG_FLUSH();
            return 1;
        }
    }
    defer(UnloadVulkanState(&vulkanState));
    LOG_INFO("Loaded Vulkan state, %llu swapchain images\n", vulkanState.swapchainImages.size);

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

    running_ = true;
    while (running_) {
        Win32ProcessMessages(hWnd);

        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(vulkanState.device, vulkanState.swapchain, UINT64_MAX,
                                                vulkanState.imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            RECT clientRect;
            if (GetClientRect(hWnd, &clientRect)) {
                Vec2Int size = { clientRect.right, clientRect.bottom };
                LinearAllocator tempAllocator(transientMemory);

                vkDeviceWaitIdle(vulkanState.device);
                UnloadVulkanSwapchain(&vulkanState);
                RecreateVulkanSwapchain(&vulkanState, size, &tempAllocator);
                continue;
            }
            else {
                LOG_ERROR("GetClientRect failed before swapchain recreation\n");
            }
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            LOG_ERROR("Failed to acquire swapchain image\n");
        }

        const VkSemaphore waitSemaphores[] = { vulkanState.imageAvailableSemaphore };
        const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

        const VkSemaphore signalSemaphores[] = { vulkanState.renderFinishedSemaphore };

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = C_ARRAY_LENGTH(waitSemaphores);
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &vulkanState.commandBuffers[(uint64)imageIndex];
        submitInfo.signalSemaphoreCount = C_ARRAY_LENGTH(signalSemaphores);
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(vulkanState.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
            LOG_ERROR("Failed to submit draw command buffer\n");
        }

        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = C_ARRAY_LENGTH(signalSemaphores);
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &vulkanState.swapchain;
        presentInfo.pImageIndices = &imageIndex;
        presentInfo.pResults = nullptr;

        vkQueuePresentKHR(vulkanState.presentQueue, &presentInfo);

        vkQueueWaitIdle(vulkanState.graphicsQueue);
        vkQueueWaitIdle(vulkanState.presentQueue);

        // timing information
        {
            LARGE_INTEGER timerEnd;
            QueryPerformanceCounter(&timerEnd);
            uint64 cyclesEnd = __rdtsc();

            int64 timerElapsed = timerEnd.QuadPart - timerLast.QuadPart;
            float64 elapsedMs = (float64)timerElapsed / timerFreq * 1000.0f;
            int64 cyclesElapsed = cyclesEnd - cyclesLast;
            float64 megaCyclesElapsed = (float64)cyclesElapsed / 1000000.0f;
            LOG_INFO("elapsed %.03f ms | %.03f MC\n", elapsedMs, megaCyclesElapsed);

            timerLast = timerEnd;
            cyclesLast = cyclesEnd;
        }
    }

    vkDeviceWaitIdle(vulkanState.device);

    return 0;
}
