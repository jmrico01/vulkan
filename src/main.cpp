#include "main.h"

#include <intrin.h>
#include <stdio.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include <km_common/km_array.h>
#include <km_common/km_defines.h>
#include <km_common/km_os.h>
#include <km_common/km_string.h>

#include "app_main.h"
#include "load_obj.h"

#define RESTRICT_LIGHTING 1
// 0, 1, 2 - some rocks
// 3 - box, left
// 4 - head
// 5 - box, right
// 6 - some other rock
// 7 - walls
const int MODELS_TO_LIGHT[] = {
    4
};

#define RESTRICT_OCCLUSION 0
#define MODEL_TO_OCCLUDE MODEL_IND_BOX

#define RESTRICT_WALL 0
const uint64 PLANE_LEFT  = 0;
const uint64 PLANE_BACK  = 1;
const uint64 PLANE_RIGHT = 2;
const uint64 PLANE_FLOOR = 3;
#define PLANE_TO_LIGHT PLANE_FLOOR

const int WINDOW_START_WIDTH  = 1600;
const int WINDOW_START_HEIGHT = 900;

const uint64 PERMANENT_MEMORY_SIZE = MEGABYTES(1);
const uint64 TRANSIENT_MEMORY_SIZE = MEGABYTES(32);

const float32 LIGHTMAP_RESOLUTION_PER_WORLD_UNIT = 128.0f;
const int LIGHTMAP_NUM_HEMISPHERE_SAMPLES = 128;
const VkFilter LIGHTMAP_TEXTURE_FILTER = VK_FILTER_LINEAR;

struct DebugTimer
{
    static bool initialized;
    static uint64 win32Freq;

    uint64 cycles;
    uint64 win32Time;
};

bool DebugTimer::initialized = false;
uint64 DebugTimer::win32Freq;

DebugTimer StartDebugTimer()
{
    if (!DebugTimer::initialized) {
        DebugTimer::initialized = true;
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        DebugTimer::win32Freq = freq.QuadPart;
    }

    DebugTimer timer;
    LARGE_INTEGER win32Time;
    QueryPerformanceCounter(&win32Time);
    timer.win32Time = win32Time.QuadPart;
    timer.cycles = __rdtsc();
    return timer;
}

void StopDebugTimer(DebugTimer* timer)
{
    LARGE_INTEGER win32End;
    QueryPerformanceCounter(&win32End);
    timer->cycles = __rdtsc() - timer->cycles;
    timer->win32Time = win32End.QuadPart - timer->win32Time;
}

void StopAndPrintDebugTimer(DebugTimer* timer)
{
    StopDebugTimer(timer);
    const float32 win32Time = (float32)timer->win32Time / DebugTimer::win32Freq * 1000.0f;
    LOG_INFO("Timer: %.03fms | %llu MC | %llu C\n", win32Time, timer->cycles / 1000000, timer->cycles);
}

struct UniformBufferObject
{
    alignas(16) Mat4 model;
    alignas(16) Mat4 view;
    alignas(16) Mat4 proj;
};

struct LightRect
{
    Vec3 origin;
    Vec3 width;
    Vec3 height;
    Vec3 color;
};

struct RaycastTriangle
{
    Vec3 v[3];
    Vec3 normal;
};

struct RaycastMesh
{
    Vec3 min, max;
    Array<RaycastTriangle> triangles;
};

struct RaycastGeometry
{
    Array<RaycastMesh> meshes;
};

struct WorkLightmapRasterizeRow
{
    int squareSize;
    uint32* pixels;
    int startPixelX, endPixelX, pixelY;
    Vertex v0, v1, v2;
    Array<Vec3> hemisphereSamples;
    const Geometry* geometry;
};

#define WORK_QUEUE_CALLBACK_FUNCTION(name) void name(void* data);
typedef WORK_QUEUE_CALLBACK_FUNCTION(WorkQueueCallbackFunction);

struct WorkEntry
{
    WorkQueueCallbackFunction callback;
    void* data;
};

struct WorkQueue
{
    static const uint64 MAX_ENTRIES = 
};

struct WorkerThread
{
    HANDLE handle;
};

DWORD WINAPI WorkerThreadProc(_In_ LPVOID lpParameter)
{
    WorkQueue* queue = (WorkQueue*)lpParameter;
}

bool SpawnWorkerThread(WorkQueue* queue, WorkerThread* thread)
{
    HANDLE handle = CreateThread(NULL, 0, WorkerThreadProc, queue, 0, NULL);
    if (handle == NULL) {
        LOG_ERROR("Failed to spawn worker thread\n");
        return false;
    }

    thread->handle = handle;
    return true;
}

void AddWork(WorkQueue* queue, WorkQueueCallbackFunction callback, void* data)
{
}

internal AppState* GetAppState(AppMemory* memory)
{
    DEBUG_ASSERT(sizeof(AppState) < memory->permanent.size);
    return (AppState*)memory->permanent.data;
}

void GenerateHemisphereSamples(int n, Vec3* samples)
{
    for (int i = 0; i < n; i++) {
        Vec3 dir;
        do {
            dir.x = RandFloat32();
            dir.y = RandFloat32(-1.0f, 1.0f);
            dir.z = RandFloat32(-1.0f, 1.0f);
        } while (MagSq(dir) > 1.0f);

        samples[i] = Normalize(dir);
    }
}

internal Vec3 RaycastColor(int numSamples, const Vec3* samples, Vec3 pos, Vec3 normal, RaycastGeometry geometry)
{
    // For reference: left wall is at  Y =  1.498721
    //                right wall is at Y = -1.544835
    const LightRect LIGHT_RECTS[] = {
        {
            .origin = { 4.0f, 1.498721f - 0.005f, 2.24f },
            .width = { -2.0f, 0.0f, 0.0f },
            .height = { 0.0f, 0.0f, -2.2f },
            .color = Vec3 { 1.0f, 0.0f, 0.0f }
        },
        {
            .origin = { 2.0f, -1.544835f + 0.005f, 2.24f },
            .width = { 2.0f, 0.0f, 0.0f },
            .height = { 0.0f, 0.0f, -2.2f },
            .color = Vec3 { 0.0f, 0.0f, 1.0f }
        },
    };

    // NOTE this will do undefined things with "up" direction
    const Quat xToNormalRot = QuatRotBetweenVectors(Vec3::unitX, normal);
    const Vec3 ambient = { 0.05f, 0.05f, 0.05f };

    Vec3 outputColor = ambient;
    for (int i = 0; i < numSamples; i++) {
        const Vec3 sampleNormal = xToNormalRot * samples[i];
        const Vec3 sampleNormalInv = {
            1.0f / sampleNormal.x,
            1.0f / sampleNormal.y,
            1.0f / sampleNormal.z
        };

        const float32 offset = 0.001f;
        const Vec3 originOffset = pos + sampleNormal * offset;

        float32 closestIntersectDist = 1e8;
        const RaycastTriangle* closestTriangle = nullptr;
        for (int j = 0; j < geometry.meshes.size; j++) {
#if RESTRICT_OCCLUSION
            if (j != MODEL_TO_OCCLUDE) continue;
#endif
            const RaycastMesh& mesh = geometry.meshes[j];
            if (!RayAxisAlignedBoxIntersection(originOffset, sampleNormalInv, mesh.min, mesh.max)) {
                continue;
            }

            for (int k = 0; k < geometry.meshes[j].triangles.size; k++) {
                const RaycastTriangle& triangle = geometry.meshes[j].triangles[k];
                float32 dot = Dot(sampleNormal, triangle.normal);
                if (dot > 0.0f) continue; // Triangle facing in the same direction as normal (away from ray)

                float32 t;
                if (RayTriangleIntersection(originOffset, sampleNormal, triangle.v[0], triangle.v[1], triangle.v[2], &t)) {
                    if (t > 0.0f && t < closestIntersectDist) {
                        closestIntersectDist = t;
                        closestTriangle = &triangle;
                    }
                }
            }
        }

        for (int l = 0; l < C_ARRAY_LENGTH(LIGHT_RECTS); l++) {
            const Vec3 lightRectNormal = Normalize(Cross(LIGHT_RECTS[l].width, LIGHT_RECTS[l].height));
            const float32 lightRectWidth = Mag(LIGHT_RECTS[l].width);
            const Vec3 lightRectUnitWidth = LIGHT_RECTS[l].width / lightRectWidth;
            const float32 lightRectHeight = Mag(LIGHT_RECTS[l].height);
            const Vec3 lightRectUnitHeight = LIGHT_RECTS[l].height / lightRectHeight;

            float32 t;
            if (!RayPlaneIntersection(pos, sampleNormal, LIGHT_RECTS[l].origin, lightRectNormal, &t)) {
                continue;
            }
            if (t < 0.0f || t > closestIntersectDist) {
                continue;
            }

            const Vec3 intersect = pos + t * sampleNormal;
            const Vec3 rectOriginToIntersect = intersect - LIGHT_RECTS[l].origin;
            const float32 projWidth = Dot(rectOriginToIntersect, lightRectUnitWidth);
            const float32 projHeight = Dot(rectOriginToIntersect, lightRectUnitHeight);
            if (0.0f <= projWidth && projWidth <= lightRectWidth && 0.0f <= projHeight && projHeight <= lightRectHeight) {
                const float32 lightIntensity = 1.0f / numSamples;
                outputColor += lightIntensity * LIGHT_RECTS[l].color;
            }
        }
    }

    outputColor.r = ClampFloat32(outputColor.r, 0.0f, 1.0f);
    outputColor.g = ClampFloat32(outputColor.g, 0.0f, 1.0f);
    outputColor.b = ClampFloat32(outputColor.b, 0.0f, 1.0f);
    return outputColor;
}

internal void CalculateLightmapForModel(const ObjModel& model, const RaycastGeometry& geometry,
                                        int squareSize, uint32* pixels)
{
    const int LIGHTMAP_PIXEL_MARGIN = 1;

    MemSet(pixels, 0, squareSize * squareSize * sizeof(uint32));

    Vec3 hemisphereSamples[LIGHTMAP_NUM_HEMISPHERE_SAMPLES];
    GenerateHemisphereSamples(LIGHTMAP_NUM_HEMISPHERE_SAMPLES, hemisphereSamples);

    for (uint64 i = 0; i < model.triangles.size; i++) {
#if RESTRICT_WALL
        if (i / 2 != PLANE_TO_LIGHT) continue;
#endif

        const Vertex v1 = model.triangles[i].v[0];
        const Vertex v2 = model.triangles[i].v[1];
        const Vertex v3 = model.triangles[i].v[2];

        const Vec2 minUv = {
            MinFloat32(v1.uv.x, MinFloat32(v2.uv.x, v3.uv.x)),
            MinFloat32(v1.uv.y, MinFloat32(v2.uv.y, v3.uv.y))
        };
        const Vec2 maxUv = {
            MaxFloat32(v1.uv.x, MaxFloat32(v2.uv.x, v3.uv.x)),
            MaxFloat32(v1.uv.y, MaxFloat32(v2.uv.y, v3.uv.y))
        };
        const int minPixelY = MaxInt((int)(minUv.y * squareSize) - LIGHTMAP_PIXEL_MARGIN, 0);
        const int maxPixelY = MinInt((int)(maxUv.y * squareSize) + LIGHTMAP_PIXEL_MARGIN, squareSize);
        for (int y = minPixelY; y < maxPixelY; y++) {
            const float32 uvY = (float32)y / squareSize;
            const float32 t1 = (uvY - v1.uv.y) / (v2.uv.y - v1.uv.y);
            const float32 x1 = v1.uv.x + (v2.uv.x - v1.uv.x) * t1;
            const float32 t2 = (uvY - v2.uv.y) / (v3.uv.y - v2.uv.y);
            const float32 x2 = v2.uv.x + (v3.uv.x - v2.uv.x) * t2;
            const float32 t3 = (uvY - v3.uv.y) / (v1.uv.y - v3.uv.y);
            const float32 x3 = v3.uv.x + (v1.uv.x - v3.uv.x) * t3;
            float32 minX = maxUv.x;
            if (x1 >= minUv.x && x1 < minX) {
                minX = x1;
            }
            if (x2 >= minUv.x && x2 < minX) {
                minX = x2;
            }
            if (x3 >= minUv.x && x3 < minX) {
                minX = x3;
            }
            float32 maxX = minUv.x;
            if (x1 <= maxUv.x && x1 > maxX) {
                maxX = x1;
            }
            if (x2 <= maxUv.x && x2 > maxX) {
                maxX = x2;
            }
            if (x3 <= maxUv.x && x3 > maxX) {
                maxX = x3;
            }
            const int minPixelX = MaxInt((int)(minX * squareSize) - LIGHTMAP_PIXEL_MARGIN, 0);
            const int maxPixelX = MinInt((int)(maxX * squareSize) + LIGHTMAP_PIXEL_MARGIN, squareSize);
            for (int x = minPixelX; x < maxPixelX; x++) {
                const float32 uvX = (float32)x / squareSize;
                const Vec3 bC = BarycentricCoordinates(Vec2 { uvX, uvY }, v1.uv, v2.uv, v3.uv);
                const Vec3 pos = v1.pos * bC.x + v2.pos * bC.y + v3.pos * bC.z;
                const Vec3 normal = v1.normal; // NOTE: we're flat-shading, so normals are all the same

                const Vec3 raycastColor = RaycastColor(LIGHTMAP_NUM_HEMISPHERE_SAMPLES, hemisphereSamples,
                                                       pos, normal, geometry);
                uint8 r = (uint8)(raycastColor.r * 255.0f);
                uint8 g = (uint8)(raycastColor.g * 255.0f);
                uint8 b = (uint8)(raycastColor.b * 255.0f);
                uint8 a = 0xff;
                pixels[y * squareSize + x] = (a << 24) + (b << 16) + (g << 8) + r;
            }
            LOG_INFO("triangle %llu/%llu: row rasterized [%d | %d | %d), %d pixels\n",
                     i + 1, model.triangles.size, minPixelY, y, maxPixelY, maxPixelX - minPixelX);
        }
    }
}

bool GenerateLightmaps(const LoadObjResult& obj, LinearAllocator* allocator)
{
    WorkQueue queue;

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    LOG_INFO("# processors: %d\n", systemInfo.dwNumberOfProcessors);
    DynamicArray<WorkerThread, LinearAllocator> threads(systemInfo.dwNumberOfProcessors, allocator);
    for (int i = 0; i < systemInfo.dwNumberOfProcessors; i++) {
        WorkerThread* thread = threads.Append();
        if (!SpawnWorkerThread(&queue, thread)) {
            return false;
        }
    }
    // TODO defer call some way of clearing queue and stopping all worker threads?
    // Could also just keep them alive and sleeping...

    RaycastGeometry geometry;
    DynamicArray<RaycastMesh, LinearAllocator> meshes(allocator);
    for (uint64 i = 0; i < obj.models.size; i++) {
        DynamicArray<RaycastTriangle, LinearAllocator> triangles(allocator);
        for (uint64 j = 0; j < obj.models[i].triangles.size; j++) {
            const MeshTriangle& triangle = obj.models[i].triangles[j];
            RaycastTriangle* newTriangle = triangles.Append();
            newTriangle->v[0] = triangle.v[0].pos;
            newTriangle->v[1] = triangle.v[1].pos;
            newTriangle->v[2] = triangle.v[2].pos;
            newTriangle->normal = triangle.v[0].normal; // NOTE flat shading, all normals are the same
        }

        RaycastMesh* mesh = meshes.Append();
        mesh->triangles = triangles.ToArray();
        mesh->min = Vec3::one * 1e8;
        mesh->max = -Vec3::one * 1e8;
        for (uint64 j = 0; j < triangles.size; j++) {
            for (int k = 0; k < 3; k++) {
                const Vec3 v = triangles[j].v[k];
                for (int e = 0; e < 3; e++) {
                    if (v.e[e] < mesh->min.e[e]) {
                        mesh->min.e[e] = v.e[e];
                    }
                    if (v.e[e] > mesh->max.e[e]) {
                        mesh->max.e[e] = v.e[e];
                    }
                }
            }
        }
    }
    geometry.meshes = meshes.ToArray();

    DebugTimer lightmapTimer = StartDebugTimer();

#if RESTRICT_LIGHTING
    for (uint64 m = 0, i = MODELS_TO_LIGHT[m]; m < C_ARRAY_LENGTH(MODELS_TO_LIGHT); i = MODELS_TO_LIGHT[m++])
#else
    for (uint64 i = 0; i < obj.models.size; i++)
#endif
    {
        auto state = allocator->SaveState();
        defer(allocator->LoadState(state));

        float32 surfaceArea = 0.0f;
        for (int j = 0; j < obj.models[i].triangles.size; j++) {
            const MeshTriangle& triangle = obj.models[i].triangles[j];
            surfaceArea += TriangleArea(triangle.v[0].pos, triangle.v[1].pos, triangle.v[2].pos);
        }

        const int size = (int)(sqrt(surfaceArea) * LIGHTMAP_RESOLUTION_PER_WORLD_UNIT);
        const int squareSize = RoundUpToPowerOfTwo(MinInt(size, 1024));
        const uint64 bytes = squareSize * squareSize * sizeof(uint32);
        uint32* pixels = (uint32*)allocator->Allocate(bytes);
        if (pixels == nullptr) {
            return false;
        }

        CalculateLightmapForModel(obj.models[i], geometry, squareSize, pixels);

        const char* filePath = ToCString(AllocPrintf(allocator, "data/lightmaps/%llu.png", i), allocator);
        stbi_write_png(filePath, squareSize, squareSize, 4, pixels, 0);
    }

    StopAndPrintDebugTimer(&lightmapTimer);

    return true;
}

APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender)
{
    UNREFERENCED_PARAMETER(audio);

    AppState* appState = GetAppState(memory);
    const Vec2Int screenSize = {
        (int)vulkanState.swapchain.extent.width,
        (int)vulkanState.swapchain.extent.height
    };

    if (!memory->initialized) {
        appState->totalElapsed = 0.0f;
        appState->cameraPos = Vec3 { -5.0f, 0.0f, 1.0f };
        appState->cameraAngles = Vec2 { 0.0f, 0.0f };

        memory->initialized = true;
    }

    if (KeyDown(input, KM_KEY_L)) {
        LinearAllocator allocator(memory->transient);

        LoadObjResult obj;
        if (LoadObj(ToString("data/models/reference-scene-small.obj"), &obj, &allocator)) {
            if (GenerateLightmaps(obj, &allocator)) {
                AppUnloadVulkanState(vulkanState, memory);
                if (!AppLoadVulkanState(vulkanState, memory)) {
                    LOG_ERROR("Failed to reload Vulkan state after lightmap generation\n");
                }
            }
            else {
                LOG_ERROR("Failed to generate lightmaps\n");
            }
        }
        else {
            LOG_ERROR("Failed to load scene .obj when generating lightmaps\n");
        }
    }

    appState->totalElapsed += deltaTime;

    UniformBufferObject ubo;
    ubo.model = Mat4::one;

    const float32 cameraSensitivity = 2.0f;
    if (MouseDown(input, KM_MOUSE_LEFT)) {
        const Vec2 mouseDeltaFrac = {
            (float32)input.mouseDelta.x / (float32)screenSize.x,
            (float32)input.mouseDelta.y / (float32)screenSize.y
        };
        appState->cameraAngles += mouseDeltaFrac * cameraSensitivity;

        appState->cameraAngles.x = ModFloat32(appState->cameraAngles.x, PI_F * 2.0f);
        appState->cameraAngles.y = ClampFloat32(appState->cameraAngles.y, -PI_F, PI_F);
    }

    const Quat cameraRotYaw = QuatFromAngleUnitAxis(appState->cameraAngles.x, Vec3::unitZ);
    const Quat cameraRotPitch = QuatFromAngleUnitAxis(appState->cameraAngles.y, Vec3::unitY);

    const Quat cameraRotYawInv = Inverse(cameraRotYaw);
    const Vec3 cameraForward = cameraRotYawInv * Vec3::unitX;
    const Vec3 cameraRight = cameraRotYawInv * -Vec3::unitY;
    const Vec3 cameraUp = Vec3::unitZ;

    const float32 speed = 2.0f;
    if (KeyDown(input, KM_KEY_W)) {
        appState->cameraPos += speed * cameraForward * deltaTime;
    }
    if (KeyDown(input, KM_KEY_S)) {
        appState->cameraPos -= speed * cameraForward * deltaTime;
    }
    if (KeyDown(input, KM_KEY_A)) {
        appState->cameraPos -= speed * cameraRight * deltaTime;
    }
    if (KeyDown(input, KM_KEY_D)) {
        appState->cameraPos += speed * cameraRight * deltaTime;
    }
    if (KeyDown(input, KM_KEY_SPACE)) {
        appState->cameraPos += speed * cameraUp * deltaTime;
    }
    if (KeyDown(input, KM_KEY_SHIFT)) {
        appState->cameraPos -= speed * cameraUp * deltaTime;
    }

    const Quat cameraRot = cameraRotPitch * cameraRotYaw;
    const Mat4 cameraRotMat4 = UnitQuatToMat4(cameraRot);

    // Transforms world-view camera (+X forward, +Z up) to Vulkan camera (+Z forward, -Y up)
    const Quat baseCameraRot = QuatFromAngleUnitAxis(-PI_F / 2.0f, Vec3::unitY)
        * QuatFromAngleUnitAxis(PI_F / 2.0f, Vec3::unitX);
    const Mat4 baseCameraRotMat4 = UnitQuatToMat4(baseCameraRot);

    ubo.view = baseCameraRotMat4 * cameraRotMat4 * Translate(-appState->cameraPos);

    const float32 aspect = (float32)screenSize.x / (float32)screenSize.y;
    const float32 nearZ = 0.1f;
    const float32 farZ = 50.0f;
    ubo.proj = Perspective(PI_F / 4.0f, aspect, nearZ, farZ);

    void* data;
    vkMapMemory(vulkanState.window.device, appState->vulkanAppState.uniformBufferMemory, 0, sizeof(ubo), 0, &data);
    MemCopy(data, &ubo, sizeof(ubo));
    vkUnmapMemory(vulkanState.window.device, appState->vulkanAppState.uniformBufferMemory);

    const VkSemaphore waitSemaphores[] = { vulkanState.window.imageAvailableSemaphore };
    const VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    const VkSemaphore signalSemaphores[] = { vulkanState.window.renderFinishedSemaphore };

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = C_ARRAY_LENGTH(waitSemaphores);
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &appState->vulkanAppState.commandBuffers[(uint64)swapchainImageIndex];
    submitInfo.signalSemaphoreCount = C_ARRAY_LENGTH(signalSemaphores);
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(vulkanState.window.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        LOG_ERROR("Failed to submit draw command buffer\n");
    }

    return true;
}

APP_LOAD_VULKAN_STATE_FUNCTION(AppLoadVulkanState)
{
    LOG_INFO("Loading Vulkan app state\n");

    const VulkanWindow& window = vulkanState.window;
    const VulkanSwapchain& swapchain = vulkanState.swapchain;

    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);
    LinearAllocator allocator(memory->transient);

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
        const Array<uint8> vertShaderCode = LoadEntireFile(ToString("data/shaders/shader.vert.spv"), &allocator);
        if (vertShaderCode.data == nullptr) {
            LOG_ERROR("Failed to load vertex shader code\n");
            return false;
        }
        const Array<uint8> fragShaderCode = LoadEntireFile(ToString("data/shaders/shader.frag.spv"), &allocator);
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
        QueueFamilyInfo queueFamilyInfo = GetQueueFamilyInfo(window.surface, window.physicalDevice, &allocator);

        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.queueFamilyIndex = queueFamilyInfo.graphicsFamilyIndex;
        poolCreateInfo.flags = 0;

        if (vkCreateCommandPool(window.device, &poolCreateInfo, nullptr, &app->commandPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateCommandPool failed\n");
            return false;
        }
    }

    LoadObjResult obj;
    if (!LoadObj(ToString("data/models/reference-scene-small.obj"), &obj, &allocator)) {
        LOG_ERROR("Failed to load reference scene .obj\n");
        return false;
    }

    // Create vertex buffer
    // Depends on commandPool and graphicsQueue, which are created by swapchain,
    // but doesn't really need to be recreated with the swapchain
    {
        uint32_t totalVertices = 0;
        for (uint64 i = 0; i < obj.models.size; i++) {
            totalVertices += (uint32_t)obj.models[i].triangles.size * 3;
        }
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
            const uint64 numBytes = obj.models[i].triangles.size * sizeof(MeshTriangle);
            MemCopy((char*)data + offset, obj.models[i].triangles.data, numBytes);
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
        poolInfo.maxSets = (uint32_t)obj.models.size;

        if (vkCreateDescriptorPool(window.device, &poolInfo, nullptr, &app->descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("vkCreateDescriptorPool failed\n");
            return false;
        }
    }

    // Create texture sampler
    {
        VkSamplerCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        createInfo.magFilter = LIGHTMAP_TEXTURE_FILTER;
        createInfo.minFilter = LIGHTMAP_TEXTURE_FILTER;
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

    // Create lightmaps
    {
        for (uint64 i = 0; i < obj.models.size; i++) {
            const char* filePath = ToCString(AllocPrintf(&allocator, "data/lightmaps/%llu.png", i), &allocator);
            int width, height, channels;
            unsigned char* imageData = stbi_load(filePath, &width, &height, &channels, 0);
            if (imageData == NULL) {
                LOG_ERROR("Failed to load lightmap: %s\n", filePath);
                return false;
            }
            defer(stbi_image_free(imageData));

            VulkanImage* image = app->lightmaps.Append();
            if (!CreateImage(window.device, window.physicalDevice, width, height,
                             VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             &image->image, &image->memory)) {
                LOG_ERROR("CreateImage failed\n");
                return false;
            }

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

            TransitionImageLayout(window.device, app->commandPool, window.graphicsQueue, image->image,
                                  VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            CopyBufferToImage(window.device, app->commandPool, window.graphicsQueue,
                              stagingBuffer, image->image, width, height);
            TransitionImageLayout(window.device, app->commandPool, window.graphicsQueue, image->image,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

            if (!CreateImageView(window.device, image->image, VK_FORMAT_R8G8B8A8_SRGB,
                                 VK_IMAGE_ASPECT_COLOR_BIT, &image->view)) {
                LOG_ERROR("CreateImageView failed\n");
                return false;
            }
        }
    }

    // Create descriptor set
    {
        FixedArray<VkDescriptorSetLayout, VulkanAppState::MAX_LIGHTMAPS> layouts;
        layouts.Clear();
        for (uint64 i = 0; i < obj.models.size; i++) {
            layouts.Append(app->descriptorSetLayout);
        }

        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = app->descriptorPool;
        allocInfo.descriptorSetCount = (uint32_t)obj.models.size;
        allocInfo.pSetLayouts = layouts.data;

        if (vkAllocateDescriptorSets(window.device, &allocInfo, app->descriptorSets.data) != VK_SUCCESS) {
            LOG_ERROR("vkAllocateDescriptorSets failed\n");
            return false;
        }
        app->descriptorSets.size = obj.models.size;

        for (uint64 i = 0; i < obj.models.size; i++) {
            VkDescriptorBufferInfo bufferInfo = {};
            bufferInfo.buffer = app->uniformBuffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkDescriptorImageInfo imageInfo = {};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = app->lightmaps[i].view;
            imageInfo.sampler = app->textureSampler;

            VkWriteDescriptorSet descriptorWrites[2] = {};
            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = app->descriptorSets[i];
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].dstArrayElement = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pBufferInfo = &bufferInfo;

            descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[1].dstSet = app->descriptorSets[i];
            descriptorWrites[1].dstBinding = 1;
            descriptorWrites[1].dstArrayElement = 0;
            descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[1].descriptorCount = 1;
            descriptorWrites[1].pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(window.device, C_ARRAY_LENGTH(descriptorWrites), descriptorWrites, 0, nullptr);
        }
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

            uint32_t vertexStart = 0;
            for (uint64 j = 0; j < obj.models.size; j++) {
                vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, app->pipelineLayout, 0, 1,
                                        &app->descriptorSets[j], 0, nullptr);

                uint32_t numVertices = (uint32_t)obj.models[j].triangles.size * 3;
                vkCmdDraw(buffer, numVertices, 1, vertexStart, 0);
                vertexStart += numVertices;
            }

            vkCmdEndRenderPass(buffer);

            if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
                LOG_ERROR("vkEndCommandBuffer failed for command buffer %llu\n", i);
                return false;
            }
        }
    }

    return true;
}

APP_UNLOAD_VULKAN_STATE_FUNCTION(AppUnloadVulkanState)
{
    LOG_INFO("Unloading Vulkan app state\n");

    const VkDevice& device = vulkanState.window.device;
    VulkanAppState* app = &(GetAppState(memory)->vulkanAppState);

    app->descriptorSets.Clear();
    vkDestroyDescriptorPool(device, app->descriptorPool, nullptr);

    vkDestroyBuffer(device, app->uniformBuffer, nullptr);
    vkFreeMemory(device, app->uniformBufferMemory, nullptr);
    vkDestroyBuffer(device, app->vertexBuffer, nullptr);
    vkFreeMemory(device, app->vertexBufferMemory, nullptr);

    vkDestroySampler(device, app->textureSampler, nullptr);

    for (uint64 i = 0; i < app->lightmaps.size; i++) {
        vkDestroyImageView(device, app->lightmaps[i].view, nullptr);
        vkDestroyImage(device, app->lightmaps[i].image, nullptr);
        vkFreeMemory(device, app->lightmaps[i].memory, nullptr);
    }
    app->lightmaps.Clear();

    app->commandBuffers.Clear();
    vkDestroyCommandPool(device, app->commandPool, nullptr);
    vkDestroyPipeline(device, app->graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(device, app->pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, app->descriptorSetLayout, nullptr);
}

#include "load_obj.cpp"
#include "vulkan.cpp"

#if GAME_WIN32
#include "win32_main.cpp"
#else
#error "Unsupported platform"
#endif

#include <km_common/km_array.cpp>
#include <km_common/km_container.cpp>
#include <km_common/km_input.cpp>
#include <km_common/km_memory.cpp>
#include <km_common/km_os.cpp>
#include <km_common/km_string.cpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>
#undef STB_SPRINTF_IMPLEMENTATION
