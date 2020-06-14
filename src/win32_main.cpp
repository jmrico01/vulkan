#include <Windows.h>
#include <vulkan/vulkan.h>

#include <km_common/km_lib.h>
#include <km_common/km_string.h>

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
};

QueueFamilyInfo GetQueueFamilyInfo(VkPhysicalDevice device, LinearAllocator* allocator)
{
    QueueFamilyInfo info;
    info.hasGraphicsFamily = false;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    DynamicArray<VkQueueFamilyProperties, LinearAllocator> queueFamilies(queueFamilyCount, allocator);
    queueFamilies.size = queueFamilyCount;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data);

    for (uint64 i = 0; i < queueFamilies.size; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            info.hasGraphicsFamily = true;
            info.graphicsFamilyIndex = (uint32_t)i;
        }
    }

    return info;
}

bool IsPhysicalDeviceSuitable(VkPhysicalDevice device, LinearAllocator* allocator)
{
    VkPhysicalDeviceProperties deviceProperties;
    vkGetPhysicalDeviceProperties(device, &deviceProperties);

    VkPhysicalDeviceFeatures deviceFeatures;
    vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

    return deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && deviceFeatures.geometryShader;
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

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    {
        LinearAllocator allocator(transientMemory.size, transientMemory.data);

        // Verify required layers
        const char* requiredLayers[] = {
            "VK_LAYER_KHRONOS_validation",
        };
        {
            uint32_t count;
            if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
                LOG_ERROR("vkEnumerateInstanceLayerProperties failed\n");
                return 1;
            }

            DynamicArray<VkLayerProperties, LinearAllocator> layers(count, &allocator);
            layers.size = count;
            if (vkEnumerateInstanceLayerProperties(&count, layers.data) != VK_SUCCESS) {
                LOG_ERROR("vkEnumerateInstanceLayerProperties failed\n");
                return 1;
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
                    LOG_ERROR("Required Vulkan layer not found: %.*s\n", (int)requiredLayer.size, requiredLayer.data);
                    return 1;
                }
            }
        }

        // Verify required extensions
        const char* requiredExtensions[] = {
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        };
        {
            uint32_t count;
            if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
                LOG_ERROR("vkEnumerateInstanceExtensionProperties failed\n");
                return 1;
            }

            DynamicArray<VkExtensionProperties, LinearAllocator> extensions(count, &allocator);
            extensions.size = count;
            if (vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data) != VK_SUCCESS) {
                LOG_ERROR("vkEnumerateInstanceExtensionProperties failed\n");
                return 1;
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
                    LOG_ERROR("Required Vulkan extension not found: %.*s\n",
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

            if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
                LOG_ERROR("vkCreateInstance failed\n");
                return 1;
            }
        }

        // Set up debug messenger
        {
            VkDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo = {};
            debugMessengerCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugMessengerCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT; // For general debug info, add VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
            debugMessengerCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugMessengerCreateInfo.pfnUserCallback = VulkanDebugCallback;
            debugMessengerCreateInfo.pUserData = nullptr;

            auto vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
            if (vkCreateDebugUtilsMessengerEXT == nullptr) {
                LOG_ERROR("vkGetInstanceProcAddr failed for vkCreateDebugUtilsMessengerEXT\n");
                return 1;
            }

            if (vkCreateDebugUtilsMessengerEXT(instance, &debugMessengerCreateInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
                LOG_ERROR("vkCreateDebugUtilsMessengerEXT failed\n");
                return 1;
            }
        }

        // Select physical device
        {
            uint32_t deviceCount = 0;
            vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
            if (deviceCount == 0) {
                LOG_ERROR("vkEnumeratePhysicalDevices returned 0 devices - no GPUs with Vulkan support\n");
                return 1;
            }

            DynamicArray<VkPhysicalDevice, LinearAllocator> devices(deviceCount, &allocator);
            devices.size = deviceCount;
            vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data);

            for (uint64 i = 0; i < devices.size; i++) {
                if (IsPhysicalDeviceSuitable(devices[i], &allocator)) {
                    physicalDevice = devices[i];
                    break;
                }
            }

            if (physicalDevice == VK_NULL_HANDLE) {
                LOG_ERROR("Failed to find a suitable GPU for Vulkan\n");
                return 1;
            }
        }
    }

    auto vkDestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (vkDestroyDebugUtilsMessengerEXT == nullptr) {
        LOG_ERROR("vkGetInstanceProcAddr failed for vkDestroyDebugUtilsMessengerEXT\n");
        return 1;
    }

    vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    vkDestroyInstance(instance, nullptr);

    return 0;
}
