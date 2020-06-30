#include <Windows.h>

#include <km_common/km_input.h>

#include "app_main.h"
#include "vulkan.h"

bool running_ = false;
bool windowSizeChange_ = false;
AppInput* input_ = nullptr;
global_var WINDOWPLACEMENT windowPlacementPrev_ = { sizeof(windowPlacementPrev_) };

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
    {
        LinearAllocator tempAllocator(appMemory.transient);
        if (!LoadVulkanState(&vulkanState, hInstance, hWnd, windowSize, &tempAllocator)) {
            LOG_ERROR("LoadVulkanState failed\n");
            LOG_FLUSH();
            return 1;
        }
    }
    LOG_INFO("Loaded Vulkan state, %llu swapchain images\n", vulkanState.swapchain.images.size);

    if (!AppLoadVulkanState(vulkanState, &appMemory)) {
        LOG_ERROR("AppLoadVulkanState failed\n");
        LOG_FLUSH();
        return 1;
    }

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
            AppUnloadVulkanState(vulkanState, &appMemory);
            if (!ReloadVulkanWindow(&vulkanState, hInstance, hWnd, newSize, &tempAllocator)) {
                DEBUG_PANIC("Failed to reload Vulkan window\n");
            }
            if (!AppLoadVulkanState(vulkanState, &appMemory)) {
                DEBUG_PANIC("Failed to reload Vulkan app state\n");
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
            AppUnloadVulkanState(vulkanState, &appMemory);
            if (!ReloadVulkanWindow(&vulkanState, hInstance, hWnd, newSize, &tempAllocator)) {
                DEBUG_PANIC("Failed to reload Vulkan window\n");
            }
            if (!AppLoadVulkanState(vulkanState, &appMemory)) {
                DEBUG_PANIC("Failed to reload Vulkan app\n");
            }
            continue;
        }
        else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            LOG_ERROR("Failed to acquire swapchain image\n");
        }

        bool shouldRender = AppUpdateAndRender(vulkanState, imageIndex, *newInput, lastElapsed, &appMemory, &appAudio);
        // TODO don't ignore this return value

        // TODO not sure what the split in Vulkan rendering responsibility should be between
        // the OS and game layers...
        const VkSemaphore signalSemaphores[] = { vulkanState.window.renderFinishedSemaphore };

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
    AppUnloadVulkanState(vulkanState, &appMemory);
    UnloadVulkanState(&vulkanState);

    return 0;
}
