#pragma once

#undef internal // Required to build FreeType
#include <ft2build.h>
#include FT_FREETYPE_H
#define internal static

#include <km_common/km_array.h>
#include <km_common/km_math.h>
#include <km_common/km_string.h>

struct GlyphInfo
{
	Vec2Int size;
    Vec2Int offset;
    Vec2Int advance;
    Vec2 uvOrigin;
	Vec2 uvSize;
};

struct LoadFontFaceResult
{
    uint32 atlasWidth, atlasHeight;
    uint8* atlasData;

	uint32 height;
    Array<GlyphInfo> glyphInfo;
};

bool LoadFontFace(FT_Library library, const_string fontFilePath, uint32 height, LinearAllocator* allocator,
                  LoadFontFaceResult* result);
