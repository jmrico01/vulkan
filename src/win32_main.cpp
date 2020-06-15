#include <Windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <km_common/km_container.h>
#include <km_common/km_lib.h>
#include <km_common/km_math.h>
#include <km_common/km_string.h>

struct VulkanState
{
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

    static const uint32 MAX_SWAPCHAIN_IMAGES = 16;
    FixedArray<VkImage, MAX_SWAPCHAIN_IMAGES> swapchainImages;
    FixedArray<VkImageView, MAX_SWAPCHAIN_IMAGES> swapchainImageViews;
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

bool LoadVulkanState(VulkanState* state, HINSTANCE hInstance, HWND hWnd, Vec2Int windowSize,
                     LinearAllocator* allocator)
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

    // Create swapchain
    {
        SwapchainSupportInfo swapchainSupportInfo;
        GetSwapchainSupportInfo(state->surface, state->physicalDevice, &swapchainSupportInfo);

        const VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapchainSupportInfo.formats.ToArray());
        const VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapchainSupportInfo.presentModes.ToArray());
        const VkExtent2D extent = ChooseSwapExtent(swapchainSupportInfo.capabilities, windowSize);

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

    return true;
}

void UnloadVulkanState(VulkanState* state)
{
    auto vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(state->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (vkDestroyDebugUtilsMessengerEXT == nullptr) {
        LOG_ERROR("vkGetInstanceProcAddr failed for vkDestroyDebugUtilsMessengerEXT\n");
    }

    for (uint64 i = 0; i < state->swapchainImageViews.size; i++) {
        vkDestroyImageView(state->device, state->swapchainImageViews[i], nullptr);
    }
    vkDestroySwapchainKHR(state->device, state->swapchain, nullptr);
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
#if 0
        case WM_CLOSE: {
            // TODO handle this with a message?
            // running_ = false;
        } break;
        case WM_DESTROY: {
            // TODO handle this as an error?
            // running_ = false;
        } break;

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
    LinearAllocator tempAllocator(transientMemory);
    if (!LoadVulkanState(&vulkanState, hInstance, hWnd, windowSize, &tempAllocator)) {
        LOG_ERROR("LoadVulkanState failed\n");
        LOG_FLUSH();
        return 1;
    }
    defer(UnloadVulkanState(&vulkanState));

    return 0;
}
