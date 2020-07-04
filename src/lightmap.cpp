#include "lightmap.h"

#include <stb_image_write.h>
#include <time.h>

const float32 RESOLUTION_PER_WORLD_UNIT = 64.0f;
const uint32 NUM_HEMISPHERE_SAMPLES = 64;
const uint32 SAMPLES_PER_GROUP = 8;
static_assert(NUM_HEMISPHERE_SAMPLES % SAMPLES_PER_GROUP == 0);
const uint32 NUM_HEMISPHERE_SAMPLE_GROUPS = NUM_HEMISPHERE_SAMPLES / SAMPLES_PER_GROUP;

// MODEL INDICES:
// 0, 1, 2 - some rocks
// 3 - box, left
// 4 - head
// 5 - box, right
// 6 - some other rock
// 7 - walls

#define RESTRICT_LIGHTING 1
const int MODELS_TO_LIGHT[] = {
    7
};

#define RESTRICT_OCCLUSION 0
#define MODEL_TO_OCCLUDE 3

#define RESTRICT_WALL 3
const uint64 PLANE_LEFT  = 0;
const uint64 PLANE_BACK  = 1;
const uint64 PLANE_RIGHT = 2;
const uint64 PLANE_FLOOR = 3;
#define PLANE_TO_LIGHT PLANE_FLOOR

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

struct LightRect
{
    Vec3 origin;
    Vec3 width;
    Vec3 height;
    Vec3 color;
    float32 intensity;
};

struct RaycastTriangle
{
    Vec3 pos[3];
    Vec2 uvs[3];
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

void GenerateHemisphereSamples(Array<Vec3> samples)
{
    for (uint32 i = 0; i < samples.size; i++) {
        Vec3 dir;
        do {
            dir.x = RandFloat32();
            dir.y = RandFloat32(-1.0f, 1.0f);
            dir.z = RandFloat32(-1.0f, 1.0f);
        } while (MagSq(dir) > 1.0f);

        samples[i] = Normalize(dir);
    }
}

struct SampleGroup
{
    StaticArray<Vec3, SAMPLES_PER_GROUP> group;
};

// Provides a metric that is lower the closer together 2 unit direction vectors are (can be negative)
float32 HemisphereDirCloseness(Vec3 dir1, Vec3 dir2)
{
    return -Dot(dir1, dir2);
}

float32 HemisphereGroupCloseness(const Array<Vec3>& samples, int groupIndices[SAMPLES_PER_GROUP])
{
    float32 closeness = 0.0f;
    for (uint32 i = 0; i < 8; i++) {
        for (uint32 j = i + 1; j < 8; j++) {
            closeness += HemisphereDirCloseness(samples[groupIndices[i]], samples[groupIndices[j]]);
        }
    }
    return closeness;
}

void GenerateHemisphereSampleGroups(Array<SampleGroup> sampleGroups, LinearAllocator* allocator)
{
    ALLOCATOR_SCOPE_RESET(*allocator);

    Array<Vec3> samples = {
        .size = sampleGroups.size * SAMPLES_PER_GROUP,
        .data = &sampleGroups[0].group[0]
    };
    GenerateHemisphereSamples(samples);

    DEBUG_ASSERT(samples.size % 8 == 0);
    const uint32 numGroups = samples.size / 8;

    Array<int> indices = allocator->NewArray<int>(samples.size);
    for (uint32 i = 0; i < samples.size; i++) {
        indices[i] = i;
    }

    const unsigned int seed = (unsigned int)time(NULL);
    srand(seed);

    const uint32 ITERATIONS = 10000;
    Array<int> minIndices = allocator->NewArray<int>(samples.size);
    float32 minCloseness = 1e8;
    for (uint32 n = 0; n < ITERATIONS; n++) {
        indices.Shuffle();
        float32 totalCloseness = 0.0f;
        for (uint32 i = 0; i < numGroups; i++) {
            totalCloseness += HemisphereGroupCloseness(samples, &indices[i * 8]);
        }
        if (totalCloseness < minCloseness) {
            minIndices.CopyFrom(indices);
            minCloseness = totalCloseness;
        }
    }

    Array<Vec3> samplesCopy = allocator->NewArray<Vec3>(samples.size);
    samplesCopy.CopyFrom(samples);
    for (uint32 i = 0; i < samples.size; i++) {
        samples[i] = samplesCopy[minIndices[i]];
    }

    for (uint32 i = 0; i < samples.size; i++) {
        indices[i] = i;
    }
    float32 totalCloseness = 0.0f;
    for (uint32 i = 0; i < numGroups; i++) {
        totalCloseness += HemisphereGroupCloseness(samples, &indices[i * 8]);
    }
    LOG_INFO("hemisphere closeness: %f\n", totalCloseness);
}

internal Vec3 RaycastColor(Array<SampleGroup> sampleGroups, Vec3 pos, Vec3 normal, const RaycastGeometry& geometry)
{
    // For reference: left wall is at  Y =  1.498721
    //                right wall is at Y = -1.544835
    const LightRect LIGHT_RECTS[] = {
        {
            .origin = { 4.0f, 1.498721f - 0.005f, 2.24f },
            .width = { -2.0f, 0.0f, 0.0f },
            .height = { 0.0f, 0.0f, -2.2f },
            .color = Vec3 { 1.0f, 0.0f, 0.0f },
            .intensity = 2.0f
        },
        {
            .origin = { 2.0f, -1.544835f + 0.005f, 2.24f },
            .width = { 2.0f, 0.0f, 0.0f },
            .height = { 0.0f, 0.0f, -2.2f },
            .color = Vec3 { 0.0f, 0.0f, 1.0f },
            .intensity = 2.0f
        },
    };

    const uint32 numSamples = sampleGroups.size * SAMPLES_PER_GROUP;

    // NOTE this will do unknown-ish things with "up" direction
    const Quat xToNormalRot = QuatRotBetweenVectors(Vec3::unitX, normal);
    const Vec3 ambient = { 0.05f, 0.05f, 0.05f };

    Vec3 outputColor = ambient;
    for (uint32 m = 0; m < sampleGroups.size; m++) {
        // TODO hit this with some spicy AVX, 8 samples at a time
        for (uint32 n = 0; n < SAMPLES_PER_GROUP; n++) {
            const Vec3 sampleNormal = xToNormalRot * sampleGroups[m].group[n];
            const Vec3 sampleNormalInv = {
                1.0f / sampleNormal.x,
                1.0f / sampleNormal.y,
                1.0f / sampleNormal.z
            };

            const float32 offset = 0.001f;
            const Vec3 originOffset = pos + sampleNormal * offset;

            float32 closestIntersectDist = 1e8;
            const RaycastTriangle* closestTriangle = nullptr;
            for (uint32 j = 0; j < geometry.meshes.size; j++) {
#if RESTRICT_LIGHTING && RESTRICT_OCCLUSION
                if (j != MODEL_TO_OCCLUDE) continue;
#endif
                const RaycastMesh& mesh = geometry.meshes[j];
                float32 tAABB;
                if (!RayAxisAlignedBoxIntersection(originOffset, sampleNormalInv, mesh.min, mesh.max, &tAABB)) {
                    continue;
                }
                // TODO slightly untested... does this actually work?
                if (tAABB > closestIntersectDist) {
                    continue;
                }

                for (uint32 k = 0; k < geometry.meshes[j].triangles.size; k++) {
                    const RaycastTriangle& triangle = geometry.meshes[j].triangles[k];
                    float32 dot = Dot(sampleNormal, triangle.normal);
                    if (dot > 0.0f) continue; // Triangle facing in the same direction as normal (away from ray)

                    float32 t;
                    if (RayTriangleIntersection(originOffset, sampleNormal,
                                                triangle.pos[0], triangle.pos[1], triangle.pos[2], &t)) {
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
                    const float32 lightIntensity = LIGHT_RECTS[l].intensity / numSamples;
                    outputColor += lightIntensity * LIGHT_RECTS[l].color;
                }
            }
        }
    }

    outputColor.r = ClampFloat32(outputColor.r, 0.0f, 1.0f);
    outputColor.g = ClampFloat32(outputColor.g, 0.0f, 1.0f);
    outputColor.b = ClampFloat32(outputColor.b, 0.0f, 1.0f);
    return outputColor;
}

struct WorkLightmapRasterizeRowCommon
{
    int squareSize;
    Array<SampleGroup> hemisphereSampleGroups;
    const RaycastGeometry* geometry;
    uint32* pixels;
};

struct WorkLightmapRasterizeRow
{
    const WorkLightmapRasterizeRowCommon* common;
    int minPixelX, maxPixelX, pixelY;
    RaycastTriangle triangle;
};

void LightmapRasterizeRow(int squareSize, int minPixelX, int maxPixelX, int pixelY, const RaycastTriangle& triangle,
                          Array<SampleGroup> hemisphereSampleGroups, const RaycastGeometry& geometry, uint32* pixels)
{
    const float32 uvY = (float32)pixelY / squareSize;
    for (int x = minPixelX; x < maxPixelX; x++) {
        const float32 uvX = (float32)x / squareSize;
        const Vec3 bC = BarycentricCoordinates(Vec2 { uvX, uvY }, triangle.uvs[0], triangle.uvs[1], triangle.uvs[2]);
        const Vec3 p = triangle.pos[0] * bC.x + triangle.pos[1] * bC.y + triangle.pos[2] * bC.z;

        const Vec3 raycastColor = RaycastColor(hemisphereSampleGroups, p, triangle.normal, geometry);
        uint8 r = (uint8)(raycastColor.r * 255.0f);
        uint8 g = (uint8)(raycastColor.g * 255.0f);
        uint8 b = (uint8)(raycastColor.b * 255.0f);
        uint8 a = 0xff;
        pixels[pixelY * squareSize + x] = (a << 24) + (b << 16) + (g << 8) + r;
    }
}

void ThreadLightmapRasterizeRow(void* data)
{
    WorkLightmapRasterizeRow* workData = (WorkLightmapRasterizeRow*)data;
    LightmapRasterizeRow(workData->common->squareSize, workData->minPixelX, workData->maxPixelX, workData->pixelY,
                         workData->triangle, workData->common->hemisphereSampleGroups, *workData->common->geometry,
                         workData->common->pixels);
}

internal void CalculateLightmapForMesh(const RaycastGeometry& geometry, uint32 meshInd, AppWorkQueue* queue,
                                       LinearAllocator* allocator, int squareSize, uint32* pixels)
{
    const int LIGHTMAP_PIXEL_MARGIN = 1;

    MemSet(pixels, 0, squareSize * squareSize * sizeof(uint32));

    StaticArray<SampleGroup, NUM_HEMISPHERE_SAMPLE_GROUPS> hemisphereSampleGroups;
    Array<SampleGroup> hemisphereSampleGroupsArray = hemisphereSampleGroups.ToArray();
    GenerateHemisphereSampleGroups(hemisphereSampleGroupsArray, allocator);

    const WorkLightmapRasterizeRowCommon workCommon = {
        .squareSize = squareSize,
        .hemisphereSampleGroups = hemisphereSampleGroupsArray,
        .geometry = &geometry,
        .pixels = pixels
    };
    auto allocatorState = allocator->SaveState();

    const RaycastMesh& mesh = geometry.meshes[meshInd];
    for (uint32 i = 0; i < mesh.triangles.size; i++) {
#if RESTRICT_LIGHTING && RESTRICT_WALL
        if (i / 2 != PLANE_TO_LIGHT) continue;
#endif

        const RaycastTriangle& triangle = mesh.triangles[i];
        const Vec2 minUv = {
            MinFloat32(triangle.uvs[0].x, MinFloat32(triangle.uvs[1].x, triangle.uvs[2].x)),
            MinFloat32(triangle.uvs[0].y, MinFloat32(triangle.uvs[1].y, triangle.uvs[2].y))
        };
        const Vec2 maxUv = {
            MaxFloat32(triangle.uvs[0].x, MaxFloat32(triangle.uvs[1].x, triangle.uvs[2].x)),
            MaxFloat32(triangle.uvs[0].y, MaxFloat32(triangle.uvs[1].y, triangle.uvs[2].y))
        };
        const int minPixelY = MaxInt((int)(minUv.y * squareSize) - LIGHTMAP_PIXEL_MARGIN, 0);
        const int maxPixelY = MinInt((int)(maxUv.y * squareSize) + LIGHTMAP_PIXEL_MARGIN, squareSize);
        for (int y = minPixelY; y < maxPixelY; y++) {
            const float32 uvY = (float32)y / squareSize;
            const float32 t0 = (uvY - triangle.uvs[0].y) / (triangle.uvs[1].y - triangle.uvs[0].y);
            const float32 x0 = triangle.uvs[0].x + (triangle.uvs[1].x - triangle.uvs[0].x) * t0;
            const float32 t1 = (uvY - triangle.uvs[1].y) / (triangle.uvs[2].y - triangle.uvs[1].y);
            const float32 x1 = triangle.uvs[1].x + (triangle.uvs[2].x - triangle.uvs[1].x) * t1;
            const float32 t2 = (uvY - triangle.uvs[2].y) / (triangle.uvs[0].y - triangle.uvs[2].y);
            const float32 x2 = triangle.uvs[2].x + (triangle.uvs[0].x - triangle.uvs[2].x) * t2;
            float32 minX = maxUv.x;
            if (x0 >= minUv.x && x0 < minX) {
                minX = x0;
            }
            if (x1 >= minUv.x && x1 < minX) {
                minX = x1;
            }
            if (x2 >= minUv.x && x2 < minX) {
                minX = x2;
            }
            float32 maxX = minUv.x;
            if (x0 <= maxUv.x && x0 > maxX) {
                maxX = x0;
            }
            if (x1 <= maxUv.x && x1 > maxX) {
                maxX = x1;
            }
            if (x2 <= maxUv.x && x2 > maxX) {
                maxX = x2;
            }
            int minPixelX = MaxInt((int)(minX * squareSize) - LIGHTMAP_PIXEL_MARGIN, 0);
            int maxPixelX = MinInt((int)(maxX * squareSize) + LIGHTMAP_PIXEL_MARGIN, squareSize);
            int numPixels = maxPixelX - minPixelX;
            if (numPixels < 0) {
                // This happens because we give Y pixels an extra margin
                // TODO yeah, maybe this is OK
                minPixelX = (minPixelX + maxPixelX) / 2;
                maxPixelX = minPixelX + 1;
                numPixels = 1;
            }

            WorkLightmapRasterizeRow* work = allocator->New<WorkLightmapRasterizeRow>();
            if (work == nullptr) {
                CompleteAllWork(queue);
                allocator->LoadState(allocatorState);
                work = allocator->New<WorkLightmapRasterizeRow>();
                DEBUG_ASSERT(work != nullptr);
            }
            work->common = &workCommon;
            work->minPixelX = minPixelX;
            work->maxPixelX = maxPixelX;
            work->pixelY = y;
            work->triangle = triangle;
            if (!TryAddWork(queue, ThreadLightmapRasterizeRow, work)) {
                LOG_INFO("flush queue. model %lu, triangle %lu/%lu, row [%d, %d, %d)\n",
                         meshInd, i + 1, mesh.triangles.size, minPixelY, y, maxPixelY);
                CompleteAllWork(queue);
                DEBUG_ASSERT(TryAddWork(queue, ThreadLightmapRasterizeRow, work));
            }
        }
    }

    CompleteAllWork(queue);
}

bool GenerateLightmaps(const LoadObjResult& obj, AppWorkQueue* queue, LinearAllocator* allocator,
                       const char* pngPathFmt)
{
    RaycastGeometry geometry;
    DynamicArray<RaycastMesh, LinearAllocator> meshes(allocator);
    for (uint32 i = 0; i < obj.models.size; i++) {
        DynamicArray<RaycastTriangle, LinearAllocator> triangles(allocator);
        for (uint32 j = 0; j < obj.models[i].triangles.size; j++) {
            const MeshTriangle& triangle = obj.models[i].triangles[j];
            RaycastTriangle* newTriangle = triangles.Append();
            newTriangle->pos[0] = triangle.v[0].pos;
            newTriangle->pos[1] = triangle.v[1].pos;
            newTriangle->pos[2] = triangle.v[2].pos;
            newTriangle->uvs[0] = triangle.v[0].uv;
            newTriangle->uvs[1] = triangle.v[1].uv;
            newTriangle->uvs[2] = triangle.v[2].uv;
            newTriangle->normal = triangle.v[0].normal; // NOTE flat shading, all normals are the same
        }

        RaycastMesh* mesh = meshes.Append();
        mesh->triangles = triangles.ToArray();
        mesh->min = Vec3::one * 1e8;
        mesh->max = -Vec3::one * 1e8;
        for (uint32 j = 0; j < triangles.size; j++) {
            for (int k = 0; k < 3; k++) {
                const Vec3 v = triangles[j].pos[k];
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
    for (uint32 m = 0, i = MODELS_TO_LIGHT[m]; m < C_ARRAY_LENGTH(MODELS_TO_LIGHT); i = MODELS_TO_LIGHT[++m])
#else
    for (uint32 i = 0; i < geometry.meshes.size; i++)
#endif
    {
        LOG_INFO("Lighting model %lu\n", i);
        ALLOCATOR_SCOPE_RESET(*allocator);

        float32 surfaceArea = 0.0f;
        for (uint32 j = 0; j < geometry.meshes[i].triangles.size; j++) {
            const RaycastTriangle& triangle = geometry.meshes[i].triangles[j];
            surfaceArea += TriangleArea(triangle.pos[0], triangle.pos[1], triangle.pos[2]);
        }
        const int size = (int)(sqrt(surfaceArea) * RESOLUTION_PER_WORLD_UNIT);
        const int squareSize = RoundUpToPowerOfTwo(MinInt(size, 1024));

        uint32* pixels = allocator->New<uint32>(squareSize * squareSize);
        if (pixels == nullptr) {
            LOG_ERROR("Failed to allocate %dx%d pixels for lightmap %lu\n", squareSize, squareSize, i);
            return false;
        }

        CalculateLightmapForMesh(geometry, i, queue, allocator, squareSize, pixels);

        const char* filePath = ToCString(AllocPrintf(allocator, pngPathFmt, i), allocator);
        stbi_write_png(filePath, squareSize, squareSize, 4, pixels, 0);
    }

    StopAndPrintDebugTimer(&lightmapTimer);

    return true;
}
