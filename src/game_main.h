#pragma once

#define UPDATE_AND_RENDER_FUNCTION(name) void name(const GameInput& input, const ScreenInfo& screenInfo, float32 deltaTime, \
GameMemory* memory, GameAudio* audio);
typedef UPDATE_AND_RENDER_FUNCTION(UpdateAndRenderFunction);
