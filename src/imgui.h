#pragma once

#include <km_common/km_array.h>
#include <km_common/km_string.h>

const uint64 INPUT_BUFFER_MAX = 256;
using InputString = FixedArray<char, INPUT_BUFFER_MAX>;

enum class PanelRenderCommandType
{
	RECT,
	TEXT
};

using PanelRenderCommandFlags = uint8;
namespace PanelRenderCommandFlag
{
constexpr PanelRenderCommandFlags OVERRIDE_COLOR = 1 << 0;
constexpr PanelRenderCommandFlags OVERRIDE_ALPHA = 1 << 1;
}

struct PanelRenderCommandRect
{
	Vec2Int size;
};

struct PanelRenderCommandText
{
    string text; // NOTE we don't own this data
	const VulkanFontFace* fontFace;
    uint32 fontIndex;
};

struct PanelRenderCommand
{
	PanelRenderCommandType type;
    PanelRenderCommandFlags flags;
    Vec2Int position;
    float32 depth;
    Vec2 anchor;
    Vec4 color;
	union
	{
		PanelRenderCommandRect commandRect;
		PanelRenderCommandText commandText;
	};
};

using PanelFlags = uint32;
namespace PanelFlag
{
constexpr PanelFlags GROW_DOWNWARDS = 1 << 0;
constexpr PanelFlags MINIMIZED      = 1 << 1;
}

struct Panel
{
    PanelFlags flags;
	Vec2Int position;
	Vec2Int positionCurrent;
	Vec2 anchor;
	Vec2Int size;
	DynamicArray<PanelRenderCommand, LinearAllocator> renderCommands;
	const AppInput* input;
    const VulkanFontFace* fontFaceDefault;

    Panel(LinearAllocator* allocator);

    void Begin(const AppInput& input, const VulkanFontFace* fontFace, PanelFlags flags, Vec2Int position, Vec2 anchor);

    void TitleBar(const_string text, bool* minimized = nullptr, Vec4 color = Vec4::zero,
                  const VulkanFontFace* fontFace = nullptr);

	void Text(const_string text, Vec4 color = Vec4::zero, const VulkanFontFace* fontFace = nullptr);

	bool Button(const_string text, Vec4 color = Vec4::zero, const VulkanFontFace* fontFace = nullptr);
	bool Checkbox(bool* value, const_string text, Vec4 color = Vec4::zero, const VulkanFontFace* fontFace = nullptr);

    bool InputText(InputString* inputString, bool* focused, Vec4 color = Vec4::zero,
                   const VulkanFontFace* fontFace = nullptr);

	bool SliderFloat(float32* value, float32 min, float32 max, Vec4 color = Vec4::zero,
                     const VulkanFontFace* font = nullptr);

    template <uint32 S1, uint32 S2>
        void Draw(Vec2Int borderSize, Vec4 defaultColor, Vec4 backgroundColor, Vec2Int screenSize,
                  VulkanSpriteRenderState<S1>* spriteRenderState, VulkanTextRenderState<S2>* textRenderState);

    // Unimplemented ...
    int SliderInt();
	float32 InputFloat();
	int InputInt();
	Vec2 InputVec2();
	Vec3 InputVec3();
	Vec4 InputVec4();
	Vec4 InputColor();
};
