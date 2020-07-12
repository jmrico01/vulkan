#pragma once

struct RectCoordsNdc
{
    Vec2 pos;
    Vec2 size;
};

struct QueueFamilyInfo
{
    bool hasGraphicsFamily;
    uint32_t graphicsFamilyIndex;
    bool hasPresentFamily;
    uint32_t presentFamilyIndex;
};

struct VulkanImage
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};


RectCoordsNdc ToRectCoordsNdc(Vec2Int pos, Vec2Int size, Vec2Int screenSize);
RectCoordsNdc ToRectCoordsNdc(Vec2Int pos, Vec2Int size, Vec2 anchor, Vec2Int screenSize);

QueueFamilyInfo GetQueueFamilyInfo(VkSurfaceKHR surface, VkPhysicalDevice physicalDevice, LinearAllocator* allocator);

bool CreateShaderModule(const Array<uint8> code, VkDevice device, VkShaderModule* shaderModule);

bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryPropertyFlags,
                  VkDevice device, VkPhysicalDevice physicalDevice, VkBuffer* buffer, VkDeviceMemory* bufferMemory);

void CopyBuffer(VkDevice device, VkCommandPool commandPool, VkQueue queue,
                VkBuffer src, VkBuffer dst, VkDeviceSize size);

bool CreateImage(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height, VkFormat format,
                 VkImageTiling tiling, VkImageUsageFlags usageFlags, VkMemoryPropertyFlags memoryPropertyFlags,
                 VkImage* image, VkDeviceMemory* imageMemory);

void TransitionImageLayout(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkImage image,
                           VkImageLayout oldLayout, VkImageLayout newLayout);

void CopyBufferToImage(VkDevice device, VkCommandPool commandPool, VkQueue queue,
                       VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

bool CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspectFlags,
                     VkImageView* imageView);

bool LoadVulkanImage(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, VkCommandPool commandPool,
                     uint32 width, uint32 height, uint32 channels, const uint8* data, VulkanImage* image);
