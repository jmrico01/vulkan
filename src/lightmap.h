#pragma once

#include <km_common/km_memory.h>

#include "app_main.h"
#include "load_obj.h"

bool GenerateLightmaps(const LoadObjResult& obj, AppWorkQueue* queue, LinearAllocator* allocator,
                       const char* pngPathFmt);
