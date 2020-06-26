#include "vulkan.h"

#include <stb_image.h>

#include <km_common/km_container.h>
#include <km_common/km_os.h>
#include <km_common/km_string.h>

struct Vertex
{
    Vec3 pos;
    Vec3 color;
    Vec2 uv;

    static VkVertexInputBindingDescription GetBindingDescription()
    {
        VkVertexInputBindingDescription bindingDescription = {};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        return bindingDescription;
    }

    static FixedArray<VkVertexInputAttributeDescription, 3> GetAttributeDescriptions()
    {
        FixedArray<VkVertexInputAttributeDescription, 3> attributeDescriptions;
        attributeDescriptions.size = 3;

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, uv);

        return attributeDescriptions;
    }
};

struct UniformBufferObject
{
    alignas(16) Mat4 model;
    alignas(16) Mat4 view;
    alignas(16) Mat4 proj;
};

const Vertex VERTICES[] = {
    { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
    { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
    { { 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
    { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },

    { { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
    { { 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
    { { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
    { { 0.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
};

const uint16_t INDICES[] = {
    0, 1, 2, 2, 3, 0,
    3, 2, 6, 6, 7, 3,
    0, 3, 7, 7, 4, 0,
    1, 0, 4, 4, 5, 1,
    2, 1, 5, 5, 6, 2,
    5, 4, 7, 7, 6, 5,
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

    for (uint64 i = 0; i < queueFamilies.size; i++) {
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

struct SwapchainSupportInfo
{
    VkSurfaceCapabilitiesKHR capabilities;
    DynamicArray<VkSurfaceFormatKHR, LinearAllocator> formats;
    DynamicArray<VkPresentModeKHR, LinearAllocator> presentModes;
};

void GetSwapchainSupportInfo(VkSurfaceKHR surface, VkPhysicalDevice physicalDevice, SwapchainSupportInfo* supportInfo)
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

bool IsPhysicalDeviceSuitable(VkSurfaceKHR surface, VkPhysicalDevice physicalDevice,
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
    GetSwapchainSupportInfo(surface, physicalDevice, &swapchainSupport);
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

VkCommandBuffer BeginOneTimeCommands(VkDevice device, VkCommandPool commandPool)
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

void EndOneTimeCommands(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkCommandBuffer commandBuffer)
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

void TransitionImageLayout(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkImage image, VkFormat format,
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
            if (!CreateImageView(state->device, state->swapchainImages[i], surfaceFormat.format,
                                 VK_IMAGE_ASPECT_COLOR_BIT, &state->swapchainImageViews[i])) {
                LOG_ERROR("CreateImageView failed for image %llu\n", i);
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

        if (vkCreateRenderPass(state->device, &renderPassCreateInfo, nullptr, &state->renderPass) != VK_SUCCESS) {
            LOG_ERROR("vkCreateRenderPass failed\n");
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

        if (vkCreateDescriptorSetLayout(state->device, &layoutCreateInfo, nullptr,
                                        &state->descriptorSetLayout) != VK_SUCCESS) {
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

        const VkVertexInputBindingDescription bindingDescription = Vertex::GetBindingDescription();
        const auto attributeDescriptions = Vertex::GetAttributeDescriptions();

        VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo = {};
        vertexInputCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputCreateInfo.vertexBindingDescriptionCount = 1;
        vertexInputCreateInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputCreateInfo.vertexAttributeDescriptionCount = (uint32_t)attributeDescriptions.size;
        vertexInputCreateInfo.pVertexAttributeDescriptions = attributeDescriptions.data;

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
        pipelineLayoutCreateInfo.pSetLayouts = &state->descriptorSetLayout;
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
        pipelineCreateInfo.pDepthStencilState = &depthStencilCreateInfo;
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

    // Create depth resources
    {
        if (!CreateImage(state->device, state->physicalDevice,
                         state->swapchainExtent.width, state->swapchainExtent.height,
                         VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &state->depthImage, &state->depthImageMemory)) {
            LOG_ERROR("CreateImage failed\n");
            return false;
        }

        if (!CreateImageView(state->device, state->depthImage, VK_FORMAT_D32_SFLOAT,
                             VK_IMAGE_ASPECT_DEPTH_BIT, &state->depthImageView)) {
            LOG_ERROR("CreateImageView failed\n");
            return false;
        }
    }

    // Create framebuffers
    {
        state->swapchainFramebuffers.size = state->swapchainImageViews.size;
        for (uint64 i = 0; i < state->swapchainFramebuffers.size; i++) {
            VkImageView attachments[] = {
                state->swapchainImageViews[i],
                state->depthImageView
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
                          state->device, state->physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for staging buffer\n");
            return false;
        }
        defer({
                  vkDestroyBuffer(state->device, stagingBuffer, nullptr);
                  vkFreeMemory(state->device, stagingBufferMemory, nullptr);
              });

        void* data;
        vkMapMemory(state->device, stagingBufferMemory, 0, imageSize, 0, &data);
        MemCopy(data, imageData, imageSize);
        vkUnmapMemory(state->device, stagingBufferMemory);

        if (!CreateImage(state->device, state->physicalDevice, width, height,
                         VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         &state->textureImage, &state->textureImageMemory)) {
            LOG_ERROR("CreateImage failed\n");
            return false;
        }

        TransitionImageLayout(state->device, state->commandPool, state->graphicsQueue, state->textureImage,
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyBufferToImage(state->device, state->commandPool, state->graphicsQueue,
                          stagingBuffer, state->textureImage, width, height);
        TransitionImageLayout(state->device, state->commandPool, state->graphicsQueue, state->textureImage,
                              VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    }

    // Create texture image view
    {
        if (!CreateImageView(state->device, state->textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                             VK_IMAGE_ASPECT_COLOR_BIT, &state->textureImageView)) {
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

        if (vkCreateSampler(state->device, &createInfo, nullptr, &state->textureSampler) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSampler failed\n");
            return false;
        }
    }

    // Create vertex buffer
    // Depends on commandPool and graphicsQueue, which are created by swapchain,
    // but doesn't really need to be recreated with the swapchain
    {
        VkDeviceSize vertexBufferSize = sizeof(VERTICES);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          state->device, state->physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for staging buffer\n");
            return false;
        }

        if (!CreateBuffer(vertexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          state->device, state->physicalDevice, &state->vertexBuffer, &state->vertexBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for vertex buffer\n");
            return false;
        }

        // Copy vertex data from CPU into memory-mapped staging buffer
        void* data;
        vkMapMemory(state->device, stagingBufferMemory, 0, vertexBufferSize, 0, &data);
        MemCopy(data, VERTICES, vertexBufferSize);
        vkUnmapMemory(state->device, stagingBufferMemory);

        // Copy vertex data from staging buffer into GPU vertex buffer
        CopyBuffer(state->device, state->commandPool, state->graphicsQueue,
                   stagingBuffer, state->vertexBuffer, vertexBufferSize);

        vkDestroyBuffer(state->device, stagingBuffer, nullptr);
        vkFreeMemory(state->device, stagingBufferMemory, nullptr);
    }

    // Create index buffer
    {
        VkDeviceSize indexBufferSize = sizeof(INDICES);

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        if (!CreateBuffer(indexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          state->device, state->physicalDevice, &stagingBuffer, &stagingBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for staging buffer\n");
            return false;
        }

        if (!CreateBuffer(indexBufferSize,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          state->device, state->physicalDevice, &state->indexBuffer, &state->indexBufferMemory)) {
            LOG_ERROR("CreateBuffer failed for index buffer\n");
            return false;
        }

        // Copy data from CPU into memory-mapped staging buffer
        void* data;
        vkMapMemory(state->device, stagingBufferMemory, 0, indexBufferSize, 0, &data);
        MemCopy(data, INDICES, indexBufferSize);
        vkUnmapMemory(state->device, stagingBufferMemory);

        // Copy  data from staging buffer into GPU buffer
        CopyBuffer(state->device, state->commandPool, state->graphicsQueue,
                   stagingBuffer, state->indexBuffer, indexBufferSize);

        vkDestroyBuffer(state->device, stagingBuffer, nullptr);
        vkFreeMemory(state->device, stagingBufferMemory, nullptr);
    }

    // Create uniform buffer
    {
        VkDeviceSize uniformBufferSize = sizeof(UniformBufferObject);
        if (!CreateBuffer(uniformBufferSize,
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          state->device, state->physicalDevice, &state->uniformBuffer, &state->uniformBufferMemory)) {
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

        if (vkCreateDescriptorPool(state->device, &poolInfo, nullptr, &state->descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorPool failed\n");
            return false;
        }
    }

    // Create descriptor set
    {
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = state->descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &state->descriptorSetLayout;

        if (vkAllocateDescriptorSets(state->device, &allocInfo, &state->descriptorSet) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateDescriptorSets failed\n");
            return false;
        }

        VkDescriptorBufferInfo bufferInfo = {};
        bufferInfo.buffer = state->uniformBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBufferObject);

        VkDescriptorImageInfo imageInfo = {};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = state->textureImageView;
        imageInfo.sampler = state->textureSampler;

        VkWriteDescriptorSet descriptorWrites[2] = {};
        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = state->descriptorSet;
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pBufferInfo = &bufferInfo;

        descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[1].dstSet = state->descriptorSet;
        descriptorWrites[1].dstBinding = 1;
        descriptorWrites[1].dstArrayElement = 0;
        descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[1].descriptorCount = 1;
        descriptorWrites[1].pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(state->device, C_ARRAY_LENGTH(descriptorWrites), descriptorWrites, 0, nullptr);
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

            const VkClearValue clearValues[] = {
                { 0.0f, 0.0f, 0.0f, 1.0f },
                { 1.0f, 0 }
            };

            VkRenderPassBeginInfo renderPassInfo = {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = state->renderPass;
            renderPassInfo.framebuffer = state->swapchainFramebuffers[i];
            renderPassInfo.renderArea.offset = { 0, 0 };
            renderPassInfo.renderArea.extent = state->swapchainExtent;
            renderPassInfo.clearValueCount = C_ARRAY_LENGTH(clearValues);
            renderPassInfo.pClearValues = clearValues;

            vkCmdBeginRenderPass(buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state->graphicsPipeline);

            const VkBuffer vertexBuffers[] = { state->vertexBuffer };
            const VkDeviceSize offsets[] = { 0 };
            vkCmdBindVertexBuffers(buffer, 0, C_ARRAY_LENGTH(vertexBuffers), vertexBuffers, offsets);
            vkCmdBindIndexBuffer(buffer, state->indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipelineLayout, 0, 1,
                                    &state->descriptorSet, 0, nullptr);

            vkCmdDrawIndexed(buffer, C_ARRAY_LENGTH(INDICES), 1, 0, 0, 0);

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
    vkDestroyDescriptorPool(state->device, state->descriptorPool, nullptr);

    vkDestroyBuffer(state->device, state->uniformBuffer, nullptr);
    vkFreeMemory(state->device, state->uniformBufferMemory, nullptr);
    vkDestroyBuffer(state->device, state->indexBuffer, nullptr);
    vkFreeMemory(state->device, state->indexBufferMemory, nullptr);
    vkDestroyBuffer(state->device, state->vertexBuffer, nullptr);
    vkFreeMemory(state->device, state->vertexBufferMemory, nullptr);

    vkDestroySampler(state->device, state->textureSampler, nullptr);
    vkDestroyImageView(state->device, state->textureImageView, nullptr);
    vkDestroyImage(state->device, state->textureImage, nullptr);
    vkFreeMemory(state->device, state->textureImageMemory, nullptr);

    vkDestroyImageView(state->device, state->depthImageView, nullptr);
    vkDestroyImage(state->device, state->depthImage, nullptr);
    vkFreeMemory(state->device, state->depthImageMemory, nullptr);

    vkDestroyCommandPool(state->device, state->commandPool, nullptr);
    for (uint64 i = 0; i < state->swapchainFramebuffers.size; i++) {
        vkDestroyFramebuffer(state->device, state->swapchainFramebuffers[i], nullptr);
    }
    vkDestroyPipeline(state->device, state->graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(state->device, state->pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(state->device, state->descriptorSetLayout, nullptr);
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
