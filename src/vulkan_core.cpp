#include "vulkan_core.h"

#include <stb_image.h>

#include <km_common/km_container.h>
#include <km_common/km_load_obj.h>
#include <km_common/km_os.h>
#include <km_common/km_string.h>

const char* REQUIRED_LAYERS[] = {
    "VK_LAYER_KHRONOS_validation"
};

const char* REQUIRED_EXTENSIONS[] = {
    "VK_KHR_surface",
    "VK_KHR_win32_surface",
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME
};

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                          VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                          const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                          void* pUserData)
{
    UNREFERENCED_PARAMETER(pUserData);

    LOG_ERROR("Validation layer, message (severity %d, type %d): %s\n",
              messageSeverity, messageType, pCallbackData->pMessage);
    return VK_FALSE;
}

struct SwapchainSupportInfo
{
    VkSurfaceCapabilitiesKHR capabilities;
    DynamicArray<VkSurfaceFormatKHR, LinearAllocator> formats;
    DynamicArray<VkPresentModeKHR, LinearAllocator> presentModes;
};

internal void GetSwapchainSupportInfo(VkSurfaceKHR surface, VkPhysicalDevice physicalDevice,
                                      SwapchainSupportInfo* supportInfo)
{
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &supportInfo->capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    if (formatCount > 0) {
        supportInfo->formats.UpdateCapacity(formatCount);
        supportInfo->formats.size = formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, supportInfo->formats.data);
    }

    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
    if (presentModeCount > 0) {
        supportInfo->presentModes.UpdateCapacity(presentModeCount);
        supportInfo->presentModes.size = presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount,
                                                  supportInfo->presentModes.data);
    }
}

internal VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const Array<VkSurfaceFormatKHR>& availableFormats)
{
    for (uint32 i = 0; i < availableFormats.size; i++) {
        if (availableFormats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormats[i];
        }
    }

    return availableFormats[0]; // sloppy fallback
}

internal VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, Vec2Int screenSize)
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

internal bool IsPhysicalDeviceSuitable(VkSurfaceKHR surface, VkPhysicalDevice physicalDevice,
                                       const Array<const char*> requiredExtensions, LinearAllocator* allocator)
{
    QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(surface, physicalDevice, allocator);
    if (!queueFamilyInfo.hasGraphicsFamily || !queueFamilyInfo.hasPresentFamily) {
        LOG_ERROR("Surface and device missing graphics or present families\n");
        return false;
    }

    uint32_t extensionCount;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
        LOG_ERROR("vkEnumerateDeviceExtensionProperties failed\n");
        return false;
    }

    DynamicArray<VkExtensionProperties, LinearAllocator> extensions(extensionCount, allocator);
    extensions.size = extensionCount;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data) != VK_SUCCESS) {
        LOG_ERROR("vkEnumerateDeviceExtensionProperties failed\n");
        return false;
    }

    for (uint32 i = 0; i < requiredExtensions.size; i++) {
        const_string requiredExtension = ToString(requiredExtensions[i]);

        bool found = false;
        for (uint32 j = 0; j < extensions.size; j++) {
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
    GetSwapchainSupportInfo(surface, physicalDevice, &swapchainSupport);
    if (swapchainSupport.formats.size == 0 || swapchainSupport.presentModes.size == 0) {
        LOG_ERROR("Insufficient swap chain capabilities (%lu formats, %lu presentModes)\n",
                  swapchainSupport.formats.size, swapchainSupport.presentModes.size);
        return false;
    }

    return true;
}

internal VkCommandBuffer BeginOneTimeCommands(VkDevice device, VkCommandPool commandPool)
{
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

internal void EndOneTimeCommands(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkCommandBuffer commandBuffer)
{
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

QueueFamilyInfo GetQueueFamilyInfo(VkSurfaceKHR surface, VkPhysicalDevice physicalDevice, LinearAllocator* allocator)
{
    QueueFamilyInfo info;
    info.hasGraphicsFamily = false;
    info.hasPresentFamily = false;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

    DynamicArray<VkQueueFamilyProperties, LinearAllocator> queueFamilies(queueFamilyCount, allocator);
    queueFamilies.size = queueFamilyCount;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data);

    for (uint32 i = 0; i < queueFamilies.size; i++) {
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, (uint32_t)i, surface, &presentSupport);
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

bool CreateShaderModule(const Array<uint8> code, VkDevice device, VkShaderModule* shaderModule)
{
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size;
    createInfo.pCode = (const uint32_t*)code.data;

    return vkCreateShaderModule(device, &createInfo, nullptr, shaderModule) == VK_SUCCESS;
}

uint32_t FindMemoryTypeIndex(VkPhysicalDeviceMemoryProperties properties,
                             uint32_t typeFilter, VkMemoryPropertyFlags propertyFlags)
{
    uint32_t memoryTypeIndex = properties.memoryTypeCount;
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if (typeFilter & (1 << i) &&
            (properties.memoryTypes[i].propertyFlags & propertyFlags) == propertyFlags) {
            memoryTypeIndex = i;
            break;
        }
    }

    return memoryTypeIndex;
}

bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryPropertyFlags,
                  VkDevice device, VkPhysicalDevice physicalDevice, VkBuffer* buffer, VkDeviceMemory* bufferMemory)
{
    VkBufferCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.size = size;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.flags = 0;

    if (vkCreateBuffer(device, &createInfo, nullptr, buffer) != VK_SUCCESS) {
        LOG_ERROR("vkCreateBuffer failed\n");
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, *buffer, &memRequirements);

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    uint32_t memoryTypeIndex = FindMemoryTypeIndex(memProperties, memRequirements.memoryTypeBits, memoryPropertyFlags);
    if (memoryTypeIndex == memProperties.memoryTypeCount) {
        LOG_ERROR("Failed to find suitable memory type\n");
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    if (vkAllocateMemory(device, &allocInfo, nullptr, bufferMemory) != VK_SUCCESS) {
        LOG_ERROR("vkAllocateMemory failed\n");
        return false;
    }

    vkBindBufferMemory(device, *buffer, *bufferMemory, 0);

    return true;
}

void CopyBuffer(VkDevice device, VkCommandPool commandPool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size)
{
    VkCommandBuffer commandBuffer = BeginOneTimeCommands(device, commandPool);

    VkBufferCopy copyRegion = {};
    copyRegion.srcOffset = 0;
    copyRegion.dstOffset = 0;
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, src, dst, 1, &copyRegion);

    EndOneTimeCommands(device, commandPool, queue, commandBuffer);
}

bool CreateImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height, VkFormat format,
                 VkImageTiling tiling, VkImageUsageFlags usageFlags, VkMemoryPropertyFlags memoryPropertyFlags,
                 VkImage* image, VkDeviceMemory* imageMemory)
{
    VkImageCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.extent.width = width;
    createInfo.extent.height = height;
    createInfo.extent.depth = 1;
    createInfo.mipLevels = 1;
    createInfo.arrayLayers = 1;
    createInfo.format = format;
    createInfo.tiling = tiling;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    createInfo.usage = usageFlags;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.flags = 0;

    if (vkCreateImage(device, &createInfo, nullptr, image) != VK_SUCCESS) {
        LOG_ERROR("vkCreateImage failed\n");
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, *image, &memRequirements);

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    uint32_t memoryTypeIndex = FindMemoryTypeIndex(memProperties, memRequirements.memoryTypeBits, memoryPropertyFlags);
    if (memoryTypeIndex == memProperties.memoryTypeCount) {
        LOG_ERROR("Failed to find suitable memory type\n");
        return false;
    }

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    if (vkAllocateMemory(device, &allocInfo, nullptr, imageMemory) != VK_SUCCESS) {
        LOG_ERROR("vkAllocateMemory failed\n");
        return false;
    }

    vkBindImageMemory(device, *image, *imageMemory, 0);

    return true;
}

void TransitionImageLayout(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkImage image,
                           VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkCommandBuffer commandBuffer = BeginOneTimeCommands(device, commandPool);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage;
    VkPipelineStageFlags dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else {
        srcStage = 0;
        dstStage = 0;
        DEBUG_PANIC("Unsupported layout transition\n");
    }

    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    EndOneTimeCommands(device, commandPool, queue, commandBuffer);
}

void CopyBufferToImage(VkDevice device, VkCommandPool commandPool, VkQueue queue,
                       VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
    VkCommandBuffer commandBuffer = BeginOneTimeCommands(device, commandPool);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { 0, 0, 0, };
    region.imageExtent = { width, height, 1 };

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    EndOneTimeCommands(device, commandPool, queue, commandBuffer);
}

bool CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectFlags,
                     VkImageView* imageView)
{
    VkImageViewCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = image;
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = format;
    createInfo.subresourceRange.aspectMask = aspectFlags;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &createInfo, nullptr, imageView) != VK_SUCCESS) {
        LOG_ERROR("vkCreateImageView failed\n");
        return false;
    }

    return true;
}

bool LoadVulkanSwapchain(const VulkanWindow& window, Vec2Int size, VulkanSwapchain* swapchain, LinearAllocator* allocator)
{
    // Create swapchain
    {
        SwapchainSupportInfo swapchainSupportInfo;
        GetSwapchainSupportInfo(window.surface, window.physicalDevice, &swapchainSupportInfo);

        const VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapchainSupportInfo.formats.ToArray());
        const VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR; // Guaranteed to be available
        const VkExtent2D extent = ChooseSwapExtent(swapchainSupportInfo.capabilities, size);

        uint32_t imageCount = swapchainSupportInfo.capabilities.minImageCount + 1;
        if (swapchainSupportInfo.capabilities.maxImageCount > 0 &&
            imageCount > swapchainSupportInfo.capabilities.maxImageCount) {
            imageCount = swapchainSupportInfo.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = window.surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(window.surface, window.physicalDevice, allocator);
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

        if (vkCreateSwapchainKHR(window.device, &createInfo, nullptr, &swapchain->swapchain) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSwapchainKHR failed\n");
            return false;
        }

        swapchain->imageFormat = surfaceFormat.format;
        swapchain->extent = extent;
        vkGetSwapchainImagesKHR(window.device, swapchain->swapchain, &imageCount, nullptr);
        if (imageCount > VulkanSwapchain::MAX_IMAGES) {
            LOG_ERROR("Too many swapchain images: %lu\n", imageCount);
            return false;
        }
        swapchain->images.size = imageCount;
        vkGetSwapchainImagesKHR(window.device, swapchain->swapchain, &imageCount, swapchain->images.data);

        swapchain->imageViews.size = imageCount;
        for (uint32 i = 0; i < swapchain->images.size; i++) {
            if (!CreateImageView(window.device, swapchain->images[i], surfaceFormat.format,VK_IMAGE_ASPECT_COLOR_BIT,
                                 &swapchain->imageViews[i])) {
                LOG_ERROR("CreateImageView failed for image %lu\n", i);
                return false;
            }
        }
    }

    // Create render pass
    {
        VkAttachmentDescription colorAttachment = {};
        colorAttachment.format = swapchain->imageFormat;
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

        VkAttachmentDescription depthAttachment = {};
        depthAttachment.format = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef = {};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        const VkAttachmentDescription attachments[] = { colorAttachment, depthAttachment };
        VkRenderPassCreateInfo renderPassCreateInfo = {};
        renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.attachmentCount = C_ARRAY_LENGTH(attachments);
        renderPassCreateInfo.pAttachments = attachments;
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpass;
        renderPassCreateInfo.dependencyCount = 1;
        renderPassCreateInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(window.device, &renderPassCreateInfo, nullptr, &swapchain->renderPass) != VK_SUCCESS) {
            LOG_ERROR("vkCreateRenderPass failed\n");
            return false;
        }
    }

    // Create depth resources
    {
        if (!CreateImage(window.device, window.physicalDevice,
                         swapchain->extent.width, swapchain->extent.height,
                         VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &swapchain->depthImage, &swapchain->depthImageMemory)) {
            LOG_ERROR("CreateImage failed\n");
            return false;
        }

        if (!CreateImageView(window.device, swapchain->depthImage, VK_FORMAT_D32_SFLOAT,
                             VK_IMAGE_ASPECT_DEPTH_BIT, &swapchain->depthImageView)) {
            LOG_ERROR("CreateImageView failed\n");
            return false;
        }
    }

    // Create framebuffers
    {
        swapchain->framebuffers.size = swapchain->imageViews.size;
        for (uint32 i = 0; i < swapchain->framebuffers.size; i++) {
            VkImageView attachments[] = {
                swapchain->imageViews[i],
                swapchain->depthImageView
            };

            VkFramebufferCreateInfo framebufferCreateInfo = {};
            framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferCreateInfo.renderPass = swapchain->renderPass;
            framebufferCreateInfo.attachmentCount = C_ARRAY_LENGTH(attachments);
            framebufferCreateInfo.pAttachments = attachments;
            framebufferCreateInfo.width = swapchain->extent.width;
            framebufferCreateInfo.height = swapchain->extent.height;
            framebufferCreateInfo.layers = 1;

            if (vkCreateFramebuffer(window.device, &framebufferCreateInfo, nullptr,
                                    &swapchain->framebuffers[i]) != VK_SUCCESS) {
                LOG_ERROR("vkCreateFramebuffer failed for framebuffer %lu\n", i);
                return false;
            }
        }
    }

    return true;
}

void UnloadVulkanSwapchain(const VulkanWindow& window, VulkanSwapchain* swapchain)
{
    for (uint32 i = 0; i < swapchain->framebuffers.size; i++) {
        vkDestroyFramebuffer(window.device, swapchain->framebuffers[i], nullptr);
    }

    vkDestroyImageView(window.device, swapchain->depthImageView, nullptr);
    vkDestroyImage(window.device, swapchain->depthImage, nullptr);
    vkFreeMemory(window.device, swapchain->depthImageMemory, nullptr);

    vkDestroyRenderPass(window.device, swapchain->renderPass, nullptr);

    for (uint32 i = 0; i < swapchain->imageViews.size; i++) {
        vkDestroyImageView(window.device, swapchain->imageViews[i], nullptr);
    }
    vkDestroySwapchainKHR(window.device, swapchain->swapchain, nullptr);
}

bool LoadVulkanWindow(const VulkanCore& core, HINSTANCE hInstance, HWND hWnd, VulkanWindow* window,
                      LinearAllocator* allocator)
{
    // Create window surface
    {
        VkWin32SurfaceCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hwnd = hWnd;
        createInfo.hinstance = hInstance;

        if (vkCreateWin32SurfaceKHR(core.instance, &createInfo, nullptr, &window->surface) != VK_SUCCESS) {
            LOG_ERROR("vkCreateWin32SurfaceKHR failed\n");
            return false;
        }
    }

    // Select physical device and queues
    {
        const char* requiredDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };
        Array<const char*> requiredDeviceExtensionsArray = {
            .size = C_ARRAY_LENGTH(requiredDeviceExtensions),
            .data = requiredDeviceExtensions
        };

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(core.instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            LOG_ERROR("vkEnumeratePhysicalDevices returned 0 devices - no GPUs with Vulkan support\n");
            return false;
        }

        DynamicArray<VkPhysicalDevice, LinearAllocator> devices(deviceCount, allocator);
        devices.size = deviceCount;
        vkEnumeratePhysicalDevices(core.instance, &deviceCount, devices.data);

        window->physicalDevice = VK_NULL_HANDLE;
        for (uint32 i = 0; i < devices.size; i++) {
            if (IsPhysicalDeviceSuitable(window->surface, devices[i], requiredDeviceExtensionsArray, allocator)) {
                window->physicalDevice = devices[i];
                break;
            }
        }

        if (window->physicalDevice == VK_NULL_HANDLE) {
            LOG_ERROR("Failed to find a suitable GPU for Vulkan\n");
            return false;
        }

        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(window->surface, window->physicalDevice, allocator);
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
        createInfo.enabledLayerCount = C_ARRAY_LENGTH(REQUIRED_LAYERS);
        createInfo.ppEnabledLayerNames = REQUIRED_LAYERS;

        if (vkCreateDevice(window->physicalDevice, &createInfo, nullptr, &window->device) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDevice failed\n");
            return false;
        }

        vkGetDeviceQueue(window->device, queueFamilyInfo.graphicsFamilyIndex, 0, &window->graphicsQueue);
        vkGetDeviceQueue(window->device, queueFamilyInfo.presentFamilyIndex, 0, &window->presentQueue);
    }

    // Create semaphores
    {
        VkSemaphoreCreateInfo semaphoreCreateInfo = {};
        semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        if (vkCreateSemaphore(window->device, &semaphoreCreateInfo, nullptr,
                              &window->imageAvailableSemaphore) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSemaphore failed\n");
            return false;
        }
        if (vkCreateSemaphore(window->device, &semaphoreCreateInfo, nullptr,
                              &window->renderFinishedSemaphore) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSemaphore failed\n");
            return false;
        }
    }

    return true;
}

void UnloadVulkanWindow(const VulkanCore& core, VulkanWindow* window)
{
    vkDestroySemaphore(window->device, window->renderFinishedSemaphore, nullptr);
    vkDestroySemaphore(window->device, window->imageAvailableSemaphore, nullptr);

    vkDestroyDevice(window->device, nullptr);
    vkDestroySurfaceKHR(core.instance, window->surface, nullptr);
}

bool LoadVulkanCore(VulkanCore* core, LinearAllocator* allocator)
{
    // Verify required layers
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

        for (int i = 0; i < C_ARRAY_LENGTH(REQUIRED_LAYERS); i++) {
            const_string requiredLayer = ToString(REQUIRED_LAYERS[i]);

            bool found = false;
            for (uint32 j = 0; j < layers.size; j++) {
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

        for (int i = 0; i < C_ARRAY_LENGTH(REQUIRED_EXTENSIONS); i++) {
            const_string requiredExtension = ToString(REQUIRED_EXTENSIONS[i]);

            bool found = false;
            for (uint32 j = 0; j < extensions.size; j++) {
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
        createInfo.enabledLayerCount = C_ARRAY_LENGTH(REQUIRED_LAYERS);
        createInfo.ppEnabledLayerNames = REQUIRED_LAYERS;
        createInfo.enabledExtensionCount = C_ARRAY_LENGTH(REQUIRED_EXTENSIONS);
        createInfo.ppEnabledExtensionNames = REQUIRED_EXTENSIONS;

        if (vkCreateInstance(&createInfo, nullptr, &core->instance) != VK_SUCCESS) {
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

        auto vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(core->instance, "vkCreateDebugUtilsMessengerEXT");
        if (vkCreateDebugUtilsMessengerEXT == nullptr) {
            LOG_ERROR("vkGetInstanceProcAddr failed for vkCreateDebugUtilsMessengerEXT\n");
            return false;
        }

        if (vkCreateDebugUtilsMessengerEXT(core->instance, &createInfo, nullptr, &core->debugMessenger) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDebugUtilsMessengerEXT failed\n");
            return false;
        }
    }

    return true;
}

void UnloadVulkanCore(VulkanCore* core)
{
    auto vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(core->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (vkDestroyDebugUtilsMessengerEXT == nullptr) {
        LOG_ERROR("vkGetInstanceProcAddr failed for vkDestroyDebugUtilsMessengerEXT\n");
    }
    vkDestroyDebugUtilsMessengerEXT(core->instance, core->debugMessenger, nullptr);

    vkDestroyInstance(core->instance, nullptr);
}

bool ReloadVulkanSwapchain(VulkanState* state, Vec2Int size, LinearAllocator* allocator)
{
    UnloadVulkanSwapchain(state->window, &state->swapchain);

    if (!LoadVulkanSwapchain(state->window, size, &state->swapchain, allocator)) {
        return false;
    }

    return true;
}

bool ReloadVulkanWindow(VulkanState* state, HINSTANCE hInstance, HWND hWnd, Vec2Int size, LinearAllocator* allocator)
{
    UnloadVulkanSwapchain(state->window, &state->swapchain);
    UnloadVulkanWindow(state->core, &state->window);

    if (!LoadVulkanWindow(state->core, hInstance, hWnd, &state->window, allocator)) {
        return false;
    }
    if (!LoadVulkanSwapchain(state->window, size, &state->swapchain, allocator)) {
        return false;
    }

    return true;
}

bool LoadVulkanState(VulkanState* state, HINSTANCE hInstance, HWND hWnd, Vec2Int size, LinearAllocator* allocator)
{
    if (!LoadVulkanCore(&state->core, allocator)) {
        return false;
    }
    if (!LoadVulkanWindow(state->core, hInstance, hWnd, &state->window, allocator)) {
        return false;
    }
    if (!LoadVulkanSwapchain(state->window, size, &state->swapchain, allocator)) {
        return false;
    }

    return true;
}

void UnloadVulkanState(VulkanState* state)
{
    UnloadVulkanSwapchain(state->window, &state->swapchain);
    UnloadVulkanWindow(state->core, &state->window);
    UnloadVulkanCore(&state->core);
}
