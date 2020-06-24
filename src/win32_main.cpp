#include <Windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "vulkan.h"

bool running_ = false;

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

    float32 lastElapsed = 0.0f;

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

                // TODO might also want to trigger these more explicitly thru win32 messages
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

        static float32 totalElapsed = 0.0f;
        totalElapsed += lastElapsed;

        UniformBufferObject ubo;
        ubo.model = Rotate(Vec3 { 0.0f, 0.0f, totalElapsed }) * Translate(Vec3 { -0.5f, -0.5f, 0.0f });
        ubo.view = Mat4::one;
        ubo.proj = Mat4::one;

        void* data;
        vkMapMemory(vulkanState.device, vulkanState.uniformBufferMemory, 0, sizeof(ubo), 0, &data);
        MemCopy(data, &ubo, sizeof(ubo));
        vkUnmapMemory(vulkanState.device, vulkanState.uniformBufferMemory);

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
            lastElapsed = (float32)timerElapsed / timerFreq;
            float32 elapsedMs = lastElapsed * 1000.0f;
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
