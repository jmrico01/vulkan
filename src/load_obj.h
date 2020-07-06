#pragma once

#include <km_common/km_array.h>
#include <km_common/km_math.h>
#include <km_common/km_memory.h>
#include <km_common/km_string.h>

struct ObjVertex
{
    Vec3 pos;
    Vec2 uv;
};

struct ObjTriangle
{
    ObjVertex v[3];
};

struct ObjQuad
{
    ObjVertex v[4];
};

struct ObjModel
{
    Array<ObjTriangle> triangles;
    Array<ObjQuad> quads;
};

struct LoadObjResult
{
    Array<uint8> file;
    Array<ObjModel> models;
};

bool LoadObj(const_string filePath, LoadObjResult* result, LinearAllocator* allocator);
