#include "lightmap.h"

#include <intrin.h>
#include <stb_image_write.h>
#include <time.h>

// MODEL INDICES:
// 0, 1, 2 - some rocks
// 3 - box, left
// 4 - head
// 5 - box, right
// 6 - some other rock
// 7 - walls

#define RESTRICT_LIGHTING 0
const int MODELS_TO_LIGHT[] = {
    3
};

#define RESTRICT_OCCLUSION 0
#define MODEL_TO_OCCLUDE 3

#define RESTRICT_WALL 0
const uint64 PLANE_LEFT  = 0;
const uint64 PLANE_BACK  = 1;
const uint64 PLANE_RIGHT = 2;
const uint64 PLANE_FLOOR = 3;
#define PLANE_TO_LIGHT PLANE_FLOOR

const float32 RESOLUTION_PER_WORLD_UNIT = 64.0f;
const uint32 NUM_HEMISPHERE_SAMPLES = 64;
const uint32 SAMPLES_PER_GROUP = 8;
static_assert(NUM_HEMISPHERE_SAMPLES % SAMPLES_PER_GROUP == 0);
const uint32 NUM_HEMISPHERE_SAMPLE_GROUPS = NUM_HEMISPHERE_SAMPLES / SAMPLES_PER_GROUP;

// SIMD helpers ------------------------------------------------------------------------

struct Vec3_8
{
    __m256 x, y, z;
};

struct Quat_8
{
    __m256 x, y, z, w;
};

Vec3_8 Set1Vec3_8(Vec3 v)
{
    return Vec3_8 { _mm256_set1_ps(v.x), _mm256_set1_ps(v.y), _mm256_set1_ps(v.z) };
}

Vec3_8 SetVec3_8(const StaticArray<Vec3, 8>& vs)
{
    return Vec3_8 {
        .x = _mm256_set_ps(vs[7].x, vs[6].x, vs[5].x, vs[4].x, vs[3].x, vs[2].x, vs[1].x, vs[0].x),
        .y = _mm256_set_ps(vs[7].y, vs[6].y, vs[5].y, vs[4].y, vs[3].y, vs[2].y, vs[1].y, vs[0].y),
        .z = _mm256_set_ps(vs[7].z, vs[6].z, vs[5].z, vs[4].z, vs[3].z, vs[2].z, vs[1].z, vs[0].z),
    };
}

Vec3_8 Add_8(Vec3_8 v1, Vec3_8 v2)
{
    return Vec3_8 {
        .x = _mm256_add_ps(v1.x, v2.x),
        .y = _mm256_add_ps(v1.y, v2.y),
        .z = _mm256_add_ps(v1.z, v2.z),
    };
}

Vec3_8 Subtract_8(Vec3_8 v1, Vec3_8 v2)
{
    return Vec3_8 {
        .x = _mm256_sub_ps(v1.x, v2.x),
        .y = _mm256_sub_ps(v1.y, v2.y),
        .z = _mm256_sub_ps(v1.z, v2.z),
    };
}

Vec3_8 Multiply_8(Vec3_8 v, __m256 s)
{
    return Vec3_8 {
        .x = _mm256_mul_ps(v.x, s),
        .y = _mm256_mul_ps(v.y, s),
        .z = _mm256_mul_ps(v.z, s),
    };
}

Vec3_8 Multiply_8(Vec3_8 v1, Vec3_8 v2)
{
    return Vec3_8 {
        .x = _mm256_mul_ps(v1.x, v2.x),
        .y = _mm256_mul_ps(v1.y, v2.y),
        .z = _mm256_mul_ps(v1.z, v2.z),
    };
}

Vec3_8 Divide_8(Vec3_8 v, __m256 s)
{
    const __m256 sInv = _mm256_rcp_ps(s);
    return Vec3_8 {
        .x = _mm256_mul_ps(v.x, sInv),
        .y = _mm256_mul_ps(v.y, sInv),
        .z = _mm256_mul_ps(v.z, sInv),
    };
}

__m256 MagSq_8(Vec3_8 v)
{
    return _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(v.x, v.x), _mm256_mul_ps(v.y, v.y)), _mm256_mul_ps(v.z, v.z));
}
__m256 Mag_8(Vec3_8 v)
{
    return _mm256_sqrt_ps(MagSq_8(v));
}

Vec3_8 Normalize_8(Vec3_8 v)
{
    const __m256 mag = Mag_8(v);
    return Divide_8(v, mag);
}

__m256 Dot_8(Vec3_8 v1, Vec3_8 v2)
{
    return _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(v1.x, v2.x), _mm256_mul_ps(v1.y, v2.y)),
                         _mm256_mul_ps(v1.z, v2.z));
}

Vec3_8 Cross_8(Vec3_8 v1, Vec3_8 v2)
{
    return Vec3_8 {
        .x = _mm256_sub_ps(_mm256_mul_ps(v1.y, v2.z), _mm256_mul_ps(v1.z, v2.y)),
        .y = _mm256_sub_ps(_mm256_mul_ps(v1.z, v2.x), _mm256_mul_ps(v1.x, v2.z)),
        .z = _mm256_sub_ps(_mm256_mul_ps(v1.x, v2.y), _mm256_mul_ps(v1.y, v2.x)),
    };
}

Vec3_8 Inverse_8(Vec3_8 v)
{
    return Vec3_8 {
        .x = _mm256_rcp_ps(v.x),
        .y = _mm256_rcp_ps(v.y),
        .z = _mm256_rcp_ps(v.z),
    };
}

Quat_8 Set1Quat_8(Quat q)
{
    return Quat_8 {
        .x = _mm256_set1_ps(q.x),
        .y = _mm256_set1_ps(q.y),
        .z = _mm256_set1_ps(q.z),
        .w = _mm256_set1_ps(q.w)
    };
}

Quat_8 Multiply_8(Quat_8 q1, Quat_8 q2)
{
    return Quat_8 {
        .x = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(q1.w, q2.x), _mm256_mul_ps(q1.x, q2.w)),
                           _mm256_sub_ps(_mm256_mul_ps(q1.y, q2.z), _mm256_mul_ps(q1.z, q2.y))),
        .y = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(q1.w, q2.y), _mm256_mul_ps(q1.y, q2.w)),
                           _mm256_sub_ps(_mm256_mul_ps(q1.z, q2.x), _mm256_mul_ps(q1.x, q2.z))),
        .z = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(q1.w, q2.z), _mm256_mul_ps(q1.z, q2.w)),
                           _mm256_sub_ps(_mm256_mul_ps(q1.x, q2.y), _mm256_mul_ps(q1.y, q2.x))),
        .w = _mm256_sub_ps(_mm256_sub_ps(_mm256_mul_ps(q1.w, q2.w), _mm256_mul_ps(q1.x, q2.x)),
                           _mm256_add_ps(_mm256_mul_ps(q1.y, q2.y), _mm256_mul_ps(q1.z, q2.z))),
    };
}

Quat_8 Inverse_8(Quat_8 q)
{
    const __m256 zero8 = _mm256_setzero_ps();
    return Quat_8 {
        .x = _mm256_sub_ps(zero8, q.x),
        .y = _mm256_sub_ps(zero8, q.y),
        .z = _mm256_sub_ps(zero8, q.z),
        .w = q.w
    };
}

Vec3_8 Multiply_8(Quat_8 q, Vec3_8 v)
{
    const __m256 zero8 = _mm256_setzero_ps();
    Quat_8 vQuat = { v.x, v.y, v.z, zero8 };
    Quat_8 qv = Multiply_8(q, vQuat);

    Quat_8 qInv = Inverse_8(q);
    Quat_8 qvqInv = Multiply_8(qv, qInv);

    return Vec3_8 { qvqInv.x, qvqInv.y, qvqInv.z };
}

__m256 RayPlaneIntersection_8(Vec3_8 rayOrigin8, Vec3_8 rayDir8, Vec3_8 planeOrigin8, Vec3_8 planeNormal8, __m256* t8)
{
    const __m256 zero8 = _mm256_setzero_ps();

    const __m256 dotDirNormal8 = Dot_8(rayDir8, planeNormal8);
    // Set mask when dot is non-zero (otherwise, ray direction is perpendicular to plane normal, so no intersection)
    const __m256 result8 = _mm256_cmp_ps(dotDirNormal8, zero8, _CMP_NEQ_OQ);

    const __m256 invDotDirNormal8 = _mm256_rcp_ps(dotDirNormal8);
    *t8 = _mm256_mul_ps(Dot_8(Subtract_8(planeOrigin8, rayOrigin8), planeNormal8), invDotDirNormal8);
    return result8;
}

__m256 RayAxisAlignedBoxIntersection_8(Vec3_8 rayOrigin8, Vec3_8 rayDirInv8, Vec3 boxMin, Vec3 boxMax)
{
    const Vec3_8 boxMin8 = Set1Vec3_8(boxMin);
    const Vec3_8 boxMax8 = Set1Vec3_8(boxMax);

    __m256 tMin = _mm256_set1_ps(-INFINITY);
    __m256 tMax = _mm256_set1_ps(INFINITY);

    const __m256 tX1 = _mm256_mul_ps(_mm256_sub_ps(boxMin8.x, rayOrigin8.x), rayDirInv8.x);
    const __m256 tX2 = _mm256_mul_ps(_mm256_sub_ps(boxMax8.x, rayOrigin8.x), rayDirInv8.x);
    tMin = _mm256_max_ps(tMin, _mm256_min_ps(tX1, tX2));
    tMax = _mm256_min_ps(tMax, _mm256_max_ps(tX1, tX2));

    const __m256 tY1 = _mm256_mul_ps(_mm256_sub_ps(boxMin8.y, rayOrigin8.y), rayDirInv8.y);
    const __m256 tY2 = _mm256_mul_ps(_mm256_sub_ps(boxMax8.y, rayOrigin8.y), rayDirInv8.y);
    tMin = _mm256_max_ps(tMin, _mm256_min_ps(tY1, tY2));
    tMax = _mm256_min_ps(tMax, _mm256_max_ps(tY1, tY2));

    const __m256 tZ1 = _mm256_mul_ps(_mm256_sub_ps(boxMin8.z, rayOrigin8.z), rayDirInv8.z);
    const __m256 tZ2 = _mm256_mul_ps(_mm256_sub_ps(boxMax8.z, rayOrigin8.z), rayDirInv8.z);
    tMin = _mm256_max_ps(tMin, _mm256_min_ps(tZ1, tZ2));
    tMax = _mm256_min_ps(tMax, _mm256_max_ps(tZ1, tZ2));

    // NOTE: doing an ordered (O) and non-signaling (Q) compare for greater than or equals here
    // This means that if there's a NaN value, the comparison will return false, but no exception will be triggered
    __m256 result8 = _mm256_cmp_ps(tMax, tMin, _CMP_GE_OQ);
    return result8;
}

__m256 RayTriangleIntersection_8(Vec3_8 rayOrigin8, Vec3_8 rayDir8, Vec3 a, Vec3 b, Vec3 c, __m256* t8)
{
    const __m256 zero8 = _mm256_setzero_ps();
    const __m256 one8 = _mm256_set1_ps(1.0f);
    const float32 epsilon = 0.000001f;
    const __m256 epsilon8 = _mm256_set1_ps(epsilon);
    const __m256 negEpsilon8 = _mm256_set1_ps(-epsilon);

    const Vec3_8 a8 = Set1Vec3_8(a);
    const Vec3_8 b8 = Set1Vec3_8(b);
    const Vec3_8 c8 = Set1Vec3_8(c);

    const Vec3_8 ab8 = Subtract_8(b8, a8);
    const Vec3_8 ac8 = Subtract_8(c8, a8);
    const Vec3_8 h8 = Cross_8(rayDir8, ac8);
    const __m256 x8 = Dot_8(ab8, h8);
    // Result mask is set when x < -EPSILON || x > EPSILON
    __m256 result8 = _mm256_or_ps(_mm256_cmp_ps(x8, negEpsilon8, _CMP_LT_OQ), _mm256_cmp_ps(x8, epsilon8, _CMP_GT_OQ));

    const __m256 f8 = _mm256_rcp_ps(x8);
    const Vec3_8 s8 = Subtract_8(rayOrigin8, a8);
    const __m256 u8 = _mm256_mul_ps(f8, Dot_8(s8, h8));
    // Result mask is set when 0.0f <= u <= 1.0f
    result8 = _mm256_and_ps(result8, _mm256_cmp_ps(zero8, u8, _CMP_LE_OQ));
    result8 = _mm256_and_ps(result8, _mm256_cmp_ps(u8, one8, _CMP_LE_OQ));

    const Vec3_8 q8 = Cross_8(s8, ab8);
    const __m256 v8 = _mm256_mul_ps(f8, Dot_8(rayDir8, q8));
    // Result mask is set when 0.0f <= v && u + v <= 1.0f
    result8 = _mm256_and_ps(result8, _mm256_cmp_ps(zero8, v8, _CMP_LE_OQ));
    result8 = _mm256_and_ps(result8, _mm256_cmp_ps(_mm256_add_ps(u8, v8), one8, _CMP_LE_OQ));

    *t8 = _mm256_mul_ps(f8, Dot_8(ac8, q8));
    // Result mask is set when t8 >= 0.0f (otherwise, intersection point is behind the ray origin)
    // NOTE if t is 0, intersection is a line (I think)
    result8 = _mm256_and_ps(result8, _mm256_cmp_ps(*t8, zero8, _CMP_GE_OQ));
    return result8;
}

// -------------------------------------------------------------------------------------

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

    const float32 largeFloat = 1e8;
    const __m256 largeFloat8 = _mm256_set1_ps(largeFloat);
    const __m256 zero8 = _mm256_setzero_ps();
    const Vec3_8 pos8 = Set1Vec3_8(pos);
    const Quat_8 xToNormalRot8 = Set1Quat_8(xToNormalRot);
    const __m256 offset8 = _mm256_set1_ps(0.001f);

    static_assert(SAMPLES_PER_GROUP == 8);
    Vec3 outputColor = ambient;
    for (uint32 m = 0; m < sampleGroups.size; m++) {
        Vec3_8 sample8 = SetVec3_8(sampleGroups[m].group);
        Vec3_8 sampleNormal8 = Multiply_8(xToNormalRot8, sample8);
        Vec3_8 sampleNormalInv8 = Inverse_8(sampleNormal8);
        Vec3_8 originOffset8 = Add_8(pos8, Multiply_8(sampleNormal8, offset8));

        __m256 closestIntersectDist8 = largeFloat8;
        for (uint32 i = 0; i < geometry.meshes.size; i++) {
#if RESTRICT_LIGHTING && RESTRICT_OCCLUSION
            if (i != MODEL_TO_OCCLUDE) continue;
#endif
            const RaycastMesh& mesh = geometry.meshes[i];
            // TODO also return min distance, and compare with closestIntersectDist8 ?
            __m256 intersect8 = RayAxisAlignedBoxIntersection_8(originOffset8, sampleNormalInv8, mesh.min, mesh.max);
            const int allZero = _mm256_testc_ps(zero8, intersect8);
            if (allZero) {
                continue;
            }

            for (uint32 j = 0; j < mesh.triangles.size; j++) {
                const RaycastTriangle& triangle = mesh.triangles[j];
                __m256 t8;
                __m256 tIntersect8 = RayTriangleIntersection_8(originOffset8, sampleNormal8,
                                                               triangle.pos[0], triangle.pos[1], triangle.pos[2], &t8);
                t8 = _mm256_blendv_ps(largeFloat8, t8, tIntersect8);
                closestIntersectDist8 = _mm256_min_ps(closestIntersectDist8, t8);
            }
        }

        for (int l = 0; l < C_ARRAY_LENGTH(LIGHT_RECTS); l++) {
            const Vec3_8 lightRectOrigin8 = Set1Vec3_8(LIGHT_RECTS[l].origin);
            const Vec3_8 lightRectNormal8 = Set1Vec3_8(Normalize(Cross(LIGHT_RECTS[l].width, LIGHT_RECTS[l].height)));

            const Vec3_8 lightWidth8 = Set1Vec3_8(LIGHT_RECTS[l].width);
            const Vec3_8 lightHeight8 = Set1Vec3_8(LIGHT_RECTS[l].height);
            const __m256 lightRectWidth8 = Mag_8(lightWidth8);
            const Vec3_8 lightRectUnitWidth8 = Divide_8(lightWidth8, lightRectWidth8);
            const __m256 lightRectHeight8 = Mag_8(lightHeight8);
            const Vec3_8 lightRectUnitHeight8 = Divide_8(lightHeight8, lightRectHeight8);

            __m256 t8;
            __m256 pIntersect8 = RayPlaneIntersection_8(pos8, sampleNormal8, lightRectOrigin8, lightRectNormal8, &t8);

            // Pixels are lit only when 0.0f <= t < closestIntersectDist
            __m256 lit8 = _mm256_and_ps(pIntersect8, _mm256_cmp_ps(zero8, t8, _CMP_LE_OQ));
            lit8 = _mm256_and_ps(lit8, _mm256_cmp_ps(t8, closestIntersectDist8, _CMP_LT_OQ));

            const Vec3_8 intersect8 = Add_8(pos8, Multiply_8(sampleNormal8, t8));
            const Vec3_8 rectOriginToIntersect8 = Subtract_8(intersect8, lightRectOrigin8);

            const __m256 projWidth8 = Dot_8(rectOriginToIntersect8, lightRectUnitWidth8);
            lit8 = _mm256_and_ps(lit8, _mm256_cmp_ps(zero8, projWidth8, _CMP_LE_OQ));
            lit8 = _mm256_and_ps(lit8, _mm256_cmp_ps(projWidth8, lightRectWidth8, _CMP_LE_OQ));

            const __m256 projHeight8 = Dot_8(rectOriginToIntersect8, lightRectUnitHeight8);
            lit8 = _mm256_and_ps(lit8, _mm256_cmp_ps(zero8, projHeight8, _CMP_LE_OQ));
            lit8 = _mm256_and_ps(lit8, _mm256_cmp_ps(projHeight8, lightRectHeight8, _CMP_LE_OQ));

            int bitMask = _mm256_movemask_ps(lit8);
            for (int i = 0; i < 8; i++) {
                if (bitMask & (1 << i)) {
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
    uint32 meshInd;
};

struct WorkLightmapRasterizeRow
{
    const WorkLightmapRasterizeRowCommon* common;
    uint32 triangleInd;
    int minPixelX, maxPixelX, pixelY;
};

void LightmapRasterizeRow(int squareSize, int minPixelX, int maxPixelX, int pixelY, uint32 meshInd, uint32 triangleInd,
                          Array<SampleGroup> hemisphereSampleGroups, const RaycastGeometry& geometry, uint32* pixels)
{
    const RaycastTriangle& triangle = geometry.meshes[meshInd].triangles[triangleInd];
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

void ThreadLightmapRasterizeRow(AppWorkQueue* queue, void* data)
{
    WorkLightmapRasterizeRow* workData = (WorkLightmapRasterizeRow*)data;

    const uint32 remaining = queue->entriesTotal - queue->entriesComplete;
    if (remaining % 100 == 0) {
        const RaycastGeometry& geometry = *workData->common->geometry;
        const int32 meshInd = workData->common->meshInd;
        LOG_INFO("%d rows in queue | processing mesh %lu, triangle %lu/%lu, row %d (%d pixels)\n",
                 remaining, meshInd, workData->triangleInd, geometry.meshes[meshInd].triangles.size, workData->pixelY,
                 workData->maxPixelX - workData->minPixelX);
    }

    LightmapRasterizeRow(workData->common->squareSize, workData->minPixelX, workData->maxPixelX, workData->pixelY,
                         workData->common->meshInd, workData->triangleInd, workData->common->hemisphereSampleGroups,
                         *workData->common->geometry, workData->common->pixels);
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
        .pixels = pixels,
        .meshInd = meshInd
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
            work->triangleInd = i;
            work->minPixelX = minPixelX;
            work->maxPixelX = maxPixelX;
            work->pixelY = y;
            if (!TryAddWork(queue, ThreadLightmapRasterizeRow, work)) {
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
        LOG_INFO("Lighting mesh %lu\n", i);
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
