#include <stdio.h>
#include <time.h>

#define LOG_ERROR(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_FLUSH() fflush(stderr); fflush(stdout)

#include <km_common/km_array.h>
#include <km_common/km_debug.h>

#include "app_main.h"
#include "lightmap.h"

// Dummies for platform main
const int WINDOW_START_WIDTH  = 1600;
const int WINDOW_START_HEIGHT = 900;
const uint64 PERMANENT_MEMORY_SIZE = MEGABYTES(1);
const uint64 TRANSIENT_MEMORY_SIZE = MEGABYTES(32);
APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender)
{
    UNREFERENCED_PARAMETER(vulkanState);
    UNREFERENCED_PARAMETER(swapchainImageIndex);
    UNREFERENCED_PARAMETER(input);
    UNREFERENCED_PARAMETER(deltaTime);
    UNREFERENCED_PARAMETER(memory);
    UNREFERENCED_PARAMETER(audio);
    UNREFERENCED_PARAMETER(queue);

    return true;
}
APP_LOAD_VULKAN_STATE_FUNCTION(AppLoadVulkanState)
{
    UNREFERENCED_PARAMETER(vulkanState);
    UNREFERENCED_PARAMETER(memory);

    return true;
}
APP_UNLOAD_VULKAN_STATE_FUNCTION(AppUnloadVulkanState)
{
    UNREFERENCED_PARAMETER(vulkanState);
    UNREFERENCED_PARAMETER(memory);
}

#include "win32_main.cpp"

int main(int argc, char* argv[])
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    srand((unsigned int)time(NULL));

    // Initialize memory
    const uint64 memoryBytes = GIGABYTES(1);
    void* memory = defaultAllocator_.Allocate(memoryBytes);
    DEBUG_ASSERT(memory != nullptr);

    // Initialize app work queue
    AppWorkQueue appWorkQueue;
    const int MAX_THREADS = 256;
    FixedArray<HANDLE, MAX_THREADS> threadHandles;
    {
        appWorkQueue.entriesTotal = 0;
        appWorkQueue.entriesComplete = 0;
        appWorkQueue.read = 0;
        appWorkQueue.write = 0;
        appWorkQueue.win32SemaphoreHandle = CreateSemaphoreEx(NULL, 0, C_ARRAY_LENGTH(appWorkQueue.entries),
                                                              NULL, 0, SEMAPHORE_ALL_ACCESS);
        if (appWorkQueue.win32SemaphoreHandle == NULL) {
            LOG_ERROR("Failed to create AppWorkQueue semaphore\n");
            LOG_FLUSH();
            return 1;
        }

        SYSTEM_INFO systemInfo;
        GetSystemInfo(&systemInfo);
        int numThreads = systemInfo.dwNumberOfProcessors - 1;
        if (numThreads > MAX_THREADS) {
            LOG_ERROR("Whoa, hello future! This machine has too many processors: %d\n", systemInfo.dwNumberOfProcessors);
            LOG_FLUSH();
            return 1;
        }
        for (int i = 0; i < numThreads; i++) {
            HANDLE* handle = threadHandles.Append();
            *handle = CreateThread(NULL, 0, WorkerThreadProc, &appWorkQueue, 0, NULL);
            if (*handle == NULL) {
                LOG_ERROR("Failed to create worker thread\n");
                LOG_FLUSH();
                return 1;
            }
        }
        LOG_INFO("Loaded work queue, %d threads\n", numThreads);
    }

    {
        LinearAllocator allocator(memoryBytes, memory);

        LoadObjResult obj;
        if (!LoadObj(ToString("data/models/reference-scene-small.obj"), &obj, &allocator)) {
            LOG_ERROR("Failed to load scene .obj when generating lightmaps\n");
            return 1;
        }
        if (!GenerateLightmaps(obj, &appWorkQueue, &allocator, "data/lightmaps/%llu.png")) {
            LOG_ERROR("Failed to generate lightmaps\n");
        }
    }
}

#include "lightmap.cpp"
#include "load_obj.cpp"
#include "vulkan.cpp"

#include <km_common/km_array.cpp>
#include <km_common/km_container.cpp>
#include <km_common/km_input.cpp>
#include <km_common/km_memory.cpp>
#include <km_common/km_os.cpp>
#include <km_common/km_string.cpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>
#undef STB_SPRINTF_IMPLEMENTATION
