#pragma once

#include <km_common/km_array.h>
#include <km_common/km_math.h>
#include <km_common/km_memory.h>
#include <km_common/km_string.h>

struct ObjModel
{
    Array<Vec3> vertices;
    Array<uint32_t> indices;
};

struct LoadObjResult
{
    Array<uint8> file;
    Array<ObjModel> models;
};

template <typename Allocator>
bool LoadObj(const_string filePath, LoadObjResult* result, Allocator* allocator);

template <typename Allocator>
void FreeObj(const LoadObjResult& objResult, Allocator* allocator);
