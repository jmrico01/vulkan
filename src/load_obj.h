#pragma once

#include <km_common/km_array.h>
#include <km_common/km_math.h>
#include <km_common/km_memory.h>
#include <km_common/km_string.h>

struct Vertex
{
    Vec3 pos;
    Vec3 normal;
};

struct ObjModel
{
    Array<Vertex> vertices;
};

struct LoadObjResult
{
    Array<uint8> file;
    Array<ObjModel> models;
};

bool LoadObj(const_string filePath, LoadObjResult* result, LinearAllocator* allocator);
