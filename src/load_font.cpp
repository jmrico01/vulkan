#include "load_font.h"

const uint32 ATLAS_DIM_MIN = 128;
const uint32 ATLAS_DIM_MAX = 2048;

const uint32 GLYPHS_TO_FILL = 256;

#if 0
#include <km_common/km_debug.h>
#include <km_common/km_string.h>

#include "main.h"

#define GLYPH_BATCH_SIZE 1024

struct AtlasData
{
    uint8 data[ATLAS_DIM_MAX * ATLAS_DIM_MAX * sizeof(uint8)];
};

struct TextDataGL
{
    Vec3 posBottomLeft[GLYPH_BATCH_SIZE];
    Vec2 size[GLYPH_BATCH_SIZE];
    Vec4 uvInfo[GLYPH_BATCH_SIZE];
};
#endif

bool LoadFontFace(FT_Library library, const_string fontFilePath, uint32 height, LinearAllocator* allocator,
                  LoadFontFaceResult* result)
{
    Array<uint8> fontFile = LoadEntireFile(fontFilePath, allocator);

    FT_Open_Args openArgs = {};
    openArgs.flags = FT_OPEN_MEMORY;
    openArgs.memory_base = (const FT_Byte*)fontFile.data;
    openArgs.memory_size = (FT_Long)fontFile.size;

    FT_Face ftFace;
    FT_Error error = FT_Open_Face(library, &openArgs, 0, &ftFace);
    if (error == FT_Err_Unknown_File_Format) {
        LOG_ERROR("Unsupported file format for %.*s\n", fontFilePath.size, fontFilePath.data);
        return false;
    }
    else if (error) {
        LOG_ERROR("Font file couldn't be read: %.*s\n", fontFilePath.size, fontFilePath.data);
        return false;
    }

    error = FT_Set_Pixel_Sizes(ftFace, 0, height);
    if (error) {
        LOG_ERROR("Failed to set font pixel size\n");
        return false;
    }

    result->glyphInfo = allocator->NewArray<GlyphInfo>(GLYPHS_TO_FILL);

    // Fill in the non-UV parameters of GlyphInfo struct array.
    for (uint32 ch = 0; ch < result->glyphInfo.size; ch++) {
        error = FT_Load_Char(ftFace, ch, FT_LOAD_RENDER);
        if (error) {
            result->glyphInfo[ch].size = Vec2Int::zero;
            result->glyphInfo[ch].offset = Vec2Int::zero;
            result->glyphInfo[ch].advance = Vec2Int::zero;
            continue;
        }
        FT_GlyphSlot glyph = ftFace->glyph;

        result->glyphInfo[ch].size = { (int)glyph->bitmap.width, (int)glyph->bitmap.rows };
        result->glyphInfo[ch].offset = { glyph->bitmap_left, (int)glyph->bitmap.rows - glyph->bitmap_top };
        result->glyphInfo[ch].advance = { glyph->advance.x, glyph->advance.y };
    }

    const uint32 pad = 2;
    // Find the lowest dimension atlas that fits all characters to be loaded
    // Atlas dimension is always a power-of-two sized square
    uint32 atlasWidth = 0;
    uint32 atlasHeight = 0;
    for (uint32 dim = ATLAS_DIM_MIN; dim <= ATLAS_DIM_MAX; dim *= 2) {
        uint32 originI = pad;
        uint32 originJ = pad;
        bool fits = true;
        for (uint32 ch = 0; ch < result->glyphInfo.size; ch++) {
            uint32 glyphWidth = result->glyphInfo[ch].size.x;
            if (originI + glyphWidth + pad >= dim) {
                originI = pad;
                originJ += height + pad;
            }
            originI += glyphWidth + pad;

            if (originJ + pad >= dim) {
                fits = false;
                break;
            }
        }
        if (fits) {
            atlasWidth = dim;
            atlasHeight = dim;
            break;
        }
    }

    if (atlasWidth == 0 || atlasHeight == 0) {
        DEBUG_PANIC("Atlas not big enough\n");
    }

    // Allocate and initialize atlas texture data
    uint8* atlasData = (uint8*)allocator->New<uint8>(atlasWidth * atlasHeight);
    if (atlasData == nullptr) {
        LOG_ERROR("Not enough memory for AtlasData, font %.*s\n", fontFilePath.size, fontFilePath.data);
        return result;
    }
    for (uint32 j = 0; j < atlasHeight; j++) {
        for (uint32 i = 0; i < atlasWidth; i++) {
            atlasData[j * atlasWidth + i] = 0;
        }
    }

    uint32 originI = pad;
    uint32 originJ = pad;
    for (uint32 ch = 0; ch < result->glyphInfo.size; ch++) {
        error = FT_Load_Char(ftFace, ch, FT_LOAD_RENDER);
        if (error) {
            continue;
        }
        FT_GlyphSlot glyph = ftFace->glyph;

        uint32 glyphWidth = glyph->bitmap.width;
        uint32 glyphHeight = glyph->bitmap.rows;
        if (originI + glyphWidth + pad >= atlasWidth) {
            originI = pad;
            originJ += height + pad;
        }

        // Write glyph bitmap into atlas.
        for (uint32 j = 0; j < glyphHeight; j++) {
            for (uint32 i = 0; i < glyphWidth; i++) {
                int indAtlas = (originJ + j) * atlasWidth + originI + i;
                int indBuffer = j * glyphWidth + i;
                atlasData[indAtlas] = glyph->bitmap.buffer[indBuffer];
            }
        }
        // Save UV coordinate data.
        result->glyphInfo[ch].uvOrigin = {
            (float32)originI / atlasWidth,
            (float32)originJ / atlasHeight
        };
        result->glyphInfo[ch].uvSize = {
            (float32)glyphWidth / atlasWidth,
            (float32)glyphHeight / atlasHeight
        };

        originI += glyphWidth + pad;
    }

    result->height = height;
    result->atlasWidth = atlasWidth;
    result->atlasHeight = atlasHeight;
    result->atlasData = atlasData;

    return true;
}

#if 0
int GetTextWidth(const FontFace& face, const_string text)
{
    float x = 0.0f;
    float y = 0.0f;
    for (uint64 i = 0; i < text.size; i++) {
        GlyphInfo glyphInfo = face.glyphInfo[text[i]];
        x += (float)glyphInfo.advanceX / 64.0f;
        y += (float)glyphInfo.advanceY / 64.0f;
    }

    return (int)x;
}

template <typename Allocator>
void DrawText(TextGL textGL, const FontFace& face, ScreenInfo screenInfo,
              const_string text, Vec2Int pos, Vec2 anchor, Vec4 color,
              Allocator* allocator)
{
    int textWidth = GetTextWidth(face, text);
    pos.x -= (int)(anchor.x * textWidth);
    pos.y -= (int)(anchor.y * face.height);

    DrawText(textGL, face, screenInfo, text, pos, color, allocator);
}
#endif
