#pragma once

#include <km_common/km_input.h>
#include <km_common/km_math.h>

struct AppMemory
{
    bool initialized;
    Array<uint8> permanent;
    Array<uint8> transient;
};

struct AppAudio
{
};

#define APP_UPDATE_AND_RENDER_FUNCTION(name) void name(const AppInput& input, Vec2Int screenSize, float32 deltaTime, \
AppMemory* memory, AppAudio* audio)
typedef APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRenderFunction);

APP_UPDATE_AND_RENDER_FUNCTION(AppUpdateAndRender);
