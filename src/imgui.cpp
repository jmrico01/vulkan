#include "imgui.h"

const float32 MARGIN_FRACTION = 1.1f;
// TODO fix, panel items depth-cull against regular world geometry, so these values have to be really small for now5
const float32 BACKGROUND_DEPTH = 0.1f;
const float32 PANEL_ITEM_DEPTH = 0.05f;
const float32 PANEL_TEXT_DEPTH = 0.0f;

void PanelInputIntState::Initialize(int value)
{
    string text = { .size = textState.MAX_LENGTH, .data = textState.text.data };
    valid = SizedPrintf(&text, "%d", value);
    if (valid) {
        textState.text.size = text.size;
        this->value = value;
    }
}

Panel::Panel(LinearAllocator* allocator)
: allocator(allocator), renderCommands(allocator)
{
}

// TODO assert that this is called before anything else?
void Panel::Begin(const AppInput& input, const VulkanFontFace* fontFace, PanelFlags flags,
                  Vec2Int position, float32 anchorX)
{
    DEBUG_ASSERT(fontFace != nullptr);

    this->flags = flags;
    this->position = position;
    this->positionCurrent = position;
    this->anchorX = anchorX;
    this->size = Vec2Int::zero;
    this->input = &input;
    this->fontFaceDefault = fontFace;
}

bool Panel::TitleBar(const_string text, bool* minimized, Vec4 color, const VulkanFontFace* fontFace)
{
    DEBUG_ASSERT(renderCommands.size == 0);

    bool changed = false;
    if (minimized == nullptr) {
        Text(text, color, fontFace);
    }
    else {
        bool notMinimized = !(*minimized);
        changed = Checkbox(&notMinimized, text, color, fontFace);
        *minimized = !notMinimized;
        if (*minimized) {
            flags |= PanelFlag::MINIMIZED;
        }
    }

    Text(const_string::empty, color, fontFaceDefault);

    return changed;
}

void Panel::Text(const_string text, Vec4 color, const VulkanFontFace* fontFace)
{
    if (flags & PanelFlag::MINIMIZED) {
        return;
    }

    const VulkanFontFace* fontToUse = fontFace == nullptr ? fontFaceDefault : fontFace;
    const int sizeX = (int)((float32)GetTextWidth(*fontToUse, text) * MARGIN_FRACTION);
    const int sizeY = (int)((float32)fontToUse->height * MARGIN_FRACTION);

    PanelRenderCommand* newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::TEXT;
    newCommand->flags = color == Vec4::zero ? 0 : PanelRenderCommandFlag::OVERRIDE_COLOR;
    newCommand->position = positionCurrent;
    if (flags & PanelFlag::GROW_DOWNWARDS) {
        newCommand->position.y += sizeY;
    }
    newCommand->color = color;
    newCommand->anchor = { anchorX, 0.0f };
    newCommand->commandText.text = ToNonConstString(text);
    newCommand->commandText.fontFace = fontToUse;

    if (flags & PanelFlag::GROW_DOWNWARDS) {
        positionCurrent.y += sizeY;
    }
    else {
        positionCurrent.y -= sizeY;
    }
    size.x = MaxInt(size.x, sizeX);
    size.y += sizeY;
}

bool Panel::Button(const_string text, Vec4 color, const VulkanFontFace* fontFace)
{
    if (flags & PanelFlag::MINIMIZED) {
        return false;
    }

    const float32 PRESSED_ALPHA = 1.0f;
    const float32 HOVERED_ALPHA = 0.65f;
    const float32 IDLE_ALPHA = 0.3f;

    const VulkanFontFace* fontToUse = fontFace == nullptr ? fontFaceDefault : fontFace;
    Vec2Int textSize = { (int)GetTextWidth(*fontToUse, text), (int)fontToUse->height };
    Vec2Int boxSize = { (int)(textSize.x * MARGIN_FRACTION), (int)(textSize.y * MARGIN_FRACTION) };
    const Vec2Int boxOffset = Vec2Int {
        Lerp(boxSize.x / 2, -boxSize.x / 2, anchorX),
        flags & PanelFlag::GROW_DOWNWARDS ? boxSize.y / 2 : -boxSize.y / 2
    };
    const Vec2Int boxCenter = positionCurrent + boxOffset;
    const RectInt boxRect = {
        .min = boxCenter - boxSize / 2,
        .max = boxCenter + boxSize / 2
    };
    const bool hovered = IsInside(input->mousePos, boxRect);
    const bool pressed = hovered && MouseDown(*input, KM_MOUSE_LEFT);
    const bool changed = hovered && MousePressed(*input, KM_MOUSE_LEFT);

    PanelRenderCommand* newCommand;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::RECT;
    newCommand->flags = PanelRenderCommandFlag::OVERRIDE_ALPHA;
    if (color != Vec4::zero) {
        newCommand->flags |= PanelRenderCommandFlag::OVERRIDE_COLOR;
    }
    newCommand->position = boxCenter;
    newCommand->anchor = Vec2 { 0.5f, 0.5f };
    newCommand->color = color;
    newCommand->color.a = pressed ? PRESSED_ALPHA : hovered ? HOVERED_ALPHA : IDLE_ALPHA;
    newCommand->commandRect.size = boxSize;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::TEXT;
    newCommand->flags = color == Vec4::zero ? 0 : PanelRenderCommandFlag::OVERRIDE_COLOR;
    newCommand->position = positionCurrent;
    if (flags & PanelFlag::GROW_DOWNWARDS) {
        newCommand->position.y += boxSize.y;
    }
    newCommand->anchor = Vec2 { anchorX, 0.0f };
    newCommand->color = color;
    newCommand->commandText.text = ToNonConstString(text);
    newCommand->commandText.fontFace = fontToUse;

    const int sizeX = (int)(boxSize.x * MARGIN_FRACTION);
    const int sizeY = (int)(boxSize.y * MARGIN_FRACTION);
    if (flags & PanelFlag::GROW_DOWNWARDS) {
        positionCurrent.y += sizeY;
    }
    else {
        positionCurrent.y -= sizeY;
    }
    size.x = MaxInt(size.x, sizeX);
    size.y += sizeY;

    return changed;
}

bool Panel::Checkbox(bool* value, const_string text, Vec4 color, const VulkanFontFace* fontFace)
{
    DEBUG_ASSERT(value != nullptr);
    if (flags & PanelFlag::MINIMIZED) {
        return false;
    }

    const float32 CHECKED_ALPHA   = 1.0f;
    const float32 HOVERED_ALPHA   = 0.65f;
    const float32 UNCHECKED_ALPHA = 0.3f;
    bool valueChanged = false;

    const VulkanFontFace* fontToUse = fontFace == nullptr ? fontFaceDefault : fontFace;
    const int fontHeight = fontToUse->height;
    const Vec2Int boxOffset = Vec2Int {
        Lerp(fontHeight / 2, -fontHeight / 2, anchorX),
        flags & PanelFlag::GROW_DOWNWARDS ? fontHeight / 2 : -fontHeight / 2
    };
    const Vec2Int boxCenter = positionCurrent + boxOffset;
    const int boxSize = (int)(fontHeight / MARGIN_FRACTION);
    const Vec2Int boxSize2 = Vec2Int { boxSize, boxSize };
    const RectInt boxRect = {
        .min = boxCenter - boxSize2 / 2,
        .max = boxCenter + boxSize2 / 2
    };
    const bool hover = IsInside(input->mousePos, boxRect);
    if (hover && MouseReleased(*input, KM_MOUSE_LEFT)) {
        *value = !(*value);
        valueChanged = true;
    }
    const float32 boxAlpha = *value ? CHECKED_ALPHA : (hover ? HOVERED_ALPHA : UNCHECKED_ALPHA);

    PanelRenderCommand* newCommand;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::RECT;
    newCommand->flags = PanelRenderCommandFlag::OVERRIDE_ALPHA;
    if (color != Vec4::zero) {
        newCommand->flags |= PanelRenderCommandFlag::OVERRIDE_COLOR;
    }
    newCommand->position = boxCenter;
    newCommand->anchor = Vec2 { 0.5f, 0.5f };
    newCommand->color = color;
    newCommand->color.a = boxAlpha;
    newCommand->commandRect.size = boxSize2;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::TEXT;
    newCommand->flags = color == Vec4::zero ? 0 : PanelRenderCommandFlag::OVERRIDE_COLOR;
    newCommand->position = positionCurrent + Vec2Int { Lerp(fontHeight, -fontHeight, anchorX), 0 };
    if (flags & PanelFlag::GROW_DOWNWARDS) {
        newCommand->position.y += boxSize2.y;
    }
    newCommand->anchor = Vec2 { anchorX, 0.0f };
    newCommand->color = color;
    newCommand->commandText.text = ToNonConstString(text);
    newCommand->commandText.fontFace = fontToUse;

    const int sizeX = (int)(GetTextWidth(*fontToUse, text) * MARGIN_FRACTION) + fontHeight;
    const int sizeY = (int)(fontHeight * MARGIN_FRACTION);
    if (flags & PanelFlag::GROW_DOWNWARDS) {
        positionCurrent.y += sizeY;
    }
    else {
        positionCurrent.y -= sizeY;
    }
    size.x = MaxInt(size.x, sizeX);
    size.y += sizeY;

    return valueChanged;
}

bool Panel::SliderFloat(PanelSliderState* state, float32 min, float32 max, const_string text, Vec4 color,
                        const VulkanFontFace* fontFace)
{
    UNREFERENCED_PARAMETER(text);

    DEBUG_ASSERT(state != nullptr);
    if (flags & PanelFlag::MINIMIZED) {
        return false;
    }

    const Vec2Int sliderBarSize = Vec2Int { 200, 5 };
    const Vec2Int sliderSize = Vec2Int { 10, 30 };
    const int totalHeight = 40;
    const float32 SLIDER_HOVER_ALPHA = 0.9f;
    const float32 SLIDER_ALPHA = 0.6f;
    const float32 BACKGROUND_BAR_ALPHA = 0.4f;

    const VulkanFontFace* fontToUse = fontFace == nullptr ? fontFaceDefault : fontFace;
    float32 sliderT = (state->value - min) / (max - min);

    bool valueChanged = false;
    RectInt sliderMouseRect;
    sliderMouseRect.min = Vec2Int {
        positionCurrent.x,
        positionCurrent.y
    };
    sliderMouseRect.max = Vec2Int {
        positionCurrent.x + sliderBarSize.x,
        positionCurrent.y + totalHeight
    };
    const bool inside = IsInside(input->mousePos, sliderMouseRect);
    if (!state->drag && inside && MouseDown(*input, KM_MOUSE_LEFT)) {
        state->drag = true;
    }
    if (state->drag && !MouseDown(*input, KM_MOUSE_LEFT)) {
        state->drag = false;
    }
    if (state->drag) {
        int newSliderX = ClampInt(input->mousePos.x - positionCurrent.x, 0, sliderBarSize.x);
        float32 newSliderT = (float32)newSliderX / sliderBarSize.x;
        state->value = newSliderT * (max - min) + min;
        valueChanged = true;
    }

    int sliderOffset = totalHeight / 2;
    Vec2Int pos = positionCurrent;
    if (flags & PanelFlag::GROW_DOWNWARDS) {
        pos.y += sliderOffset;
    }
    else {
        pos.y -= sliderOffset;
    }

    PanelRenderCommand* newCommand;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::RECT;
    newCommand->flags = PanelRenderCommandFlag::OVERRIDE_ALPHA;
    newCommand->position = pos;
    newCommand->anchor = Vec2 { 0.0f, 0.5f };
    newCommand->color = color;
    newCommand->color.a = BACKGROUND_BAR_ALPHA;
    newCommand->commandRect.size = sliderBarSize;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::RECT;
    newCommand->flags = PanelRenderCommandFlag::OVERRIDE_ALPHA;
    newCommand->position = Vec2Int {
        pos.x + (int)(sliderBarSize.x * sliderT),
        pos.y
    };
    newCommand->anchor = Vec2 { 0.0f, 0.5f };
    newCommand->color = color;
    newCommand->color.a = inside ? SLIDER_HOVER_ALPHA : SLIDER_ALPHA;
    newCommand->commandRect.size = sliderSize;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::TEXT;
    newCommand->position = Vec2Int { pos.x + sliderBarSize.x + 10, pos.y };
    newCommand->anchor = Vec2::zero;
    newCommand->color = color;
    newCommand->commandText.text = AllocPrintf(allocator, "%.03f", state->value);
    newCommand->commandText.fontFace = fontToUse;

    if (flags & PanelFlag::GROW_DOWNWARDS) {
        positionCurrent.y += totalHeight;
    }
    else {
        positionCurrent.y -= totalHeight;
    }
    size.x = MaxInt(size.x, sliderBarSize.x);
    size.y += totalHeight;

    return valueChanged;
}

bool Panel::InputText(PanelInputTextState* state, Vec4 color, const VulkanFontFace* fontFace)
{
    DEBUG_ASSERT(state != nullptr);
    if (flags & PanelFlag::MINIMIZED) {
        state->focused = false;
        return false;
    }

    bool inputChanged = false;
    if (state->focused) {
        for (uint64 i = 0; i < input->keyboardStringLen; i++) {
            char c = input->keyboardString[i];
            if (c == 8) { // backspace
                if (state->text.size > 0) {
                    state->text.RemoveLast();
                }
                inputChanged = true;
                continue;
            }
            if (c == '\n') { // enter
                state->focused = false;
                break;
            }

            if (state->text.size >= state->MAX_LENGTH) {
                continue;
            }
            inputChanged = true;
            state->text.Append(c);
        }
    }

    const float32 PRESSED_ALPHA = 0.4f;
    const float32 HOVERED_ALPHA = 0.3f;
    const float32 IDLE_ALPHA = 0.1f;
    const int BOX_MIN_WIDTH = 20;

    const VulkanFontFace* fontToUse = fontFace == nullptr ? fontFaceDefault : fontFace;
    Vec2Int textSize = { (int)GetTextWidth(*fontToUse, state->text.ToArray()), (int)fontToUse->height };
    Vec2Int boxSize = { (int)(textSize.x * MARGIN_FRACTION), (int)(textSize.y * MARGIN_FRACTION) };
    if (boxSize.x < BOX_MIN_WIDTH) {
        boxSize.x = BOX_MIN_WIDTH;
    }
    const Vec2Int boxOffset = Vec2Int {
        Lerp(boxSize.x / 2, -boxSize.x / 2, anchorX),
        flags & PanelFlag::GROW_DOWNWARDS ? boxSize.y / 2 : -boxSize.y / 2
    };
    const Vec2Int boxCenter = positionCurrent + boxOffset;
    const RectInt boxRect = {
        .min = boxCenter - boxSize / 2,
        .max = boxCenter + boxSize / 2
    };
    const bool hovered = IsInside(input->mousePos, boxRect);
    const bool pressed = hovered && MouseDown(*input, KM_MOUSE_LEFT);
    if (MousePressed(*input, KM_MOUSE_LEFT)) {
        state->focused = hovered;
    }

    PanelRenderCommand* newCommand;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::RECT;
    newCommand->flags = PanelRenderCommandFlag::OVERRIDE_ALPHA;
    if (color != Vec4::zero) {
        newCommand->flags |= PanelRenderCommandFlag::OVERRIDE_COLOR;
    }
    newCommand->position = boxCenter;
    newCommand->anchor = Vec2 { 0.5f, 0.5f };
    newCommand->color = color;
    newCommand->color.a = pressed ? PRESSED_ALPHA : hovered ? HOVERED_ALPHA : IDLE_ALPHA;
    newCommand->commandRect.size = boxSize;

    newCommand = renderCommands.Append();
    newCommand->type = PanelRenderCommandType::TEXT;
    newCommand->flags = color == Vec4::zero ? 0 : PanelRenderCommandFlag::OVERRIDE_COLOR;
    newCommand->position = positionCurrent;
    if (flags & PanelFlag::GROW_DOWNWARDS) {
        newCommand->position.y += boxSize.y;
    }
    newCommand->anchor = Vec2 { anchorX, 0.0f };
    newCommand->color = color;
    newCommand->commandText.text = state->text.ToArray();
    newCommand->commandText.fontFace = fontToUse;

    const int sizeX = (int)(boxSize.x * MARGIN_FRACTION);
    const int sizeY = (int)(boxSize.y * MARGIN_FRACTION);
    if (flags & PanelFlag::GROW_DOWNWARDS) {
        positionCurrent.y += sizeY;
    }
    else {
        positionCurrent.y -= sizeY;
    }
    size.x = MaxInt(size.x, sizeX);
    size.y += sizeY;

    return inputChanged;
}

bool Panel::InputInt(PanelInputIntState* state, Vec4 color, const VulkanFontFace* fontFace)
{
    bool inputChanged = false;
    if (InputText(&state->textState, color, fontFace)) {
        state->valid = StringToIntBase10(state->textState.text.ToArray(), &state->value);
        if (state->valid) {
            inputChanged = true;
        }
    }

    if (!state->valid) {
        // TODO a little dangerous / weird?
        renderCommands[renderCommands.size - 2].color.r = 1.0f;
        renderCommands[renderCommands.size - 2].color.g = 0.0f;
        renderCommands[renderCommands.size - 2].color.b = 0.0f;
    }

    return inputChanged;
}

template <uint32 S1, uint32 S2>
void Panel::Draw(Vec2Int borderSize, Vec4 defaultColor, Vec4 backgroundColor, Vec2Int screenSize,
                 VulkanSpriteRenderState<S1>* spriteRenderState, VulkanTextRenderState<S2>* textRenderState)
{
    // Draw background
    const Vec2Int backgroundOffset = {
        Lerp(-borderSize.x, borderSize.x, anchorX),
        flags & PanelFlag::GROW_DOWNWARDS ? -borderSize.y : borderSize.y
    };
    const Vec2Int backgroundSize = size + borderSize * 2;
    const Vec2 backgroundAnchor = { anchorX, flags & PanelFlag::GROW_DOWNWARDS ? 0.0f : 1.0f };
    PushSprite((uint32)SpriteId::PIXEL, position + backgroundOffset, backgroundSize, BACKGROUND_DEPTH,
               backgroundAnchor, backgroundColor, screenSize, spriteRenderState);

    for (uint32 i = 0; i < renderCommands.size; i++) {
        const PanelRenderCommand& command = renderCommands[i];
        Vec4 color = command.flags & PanelRenderCommandFlag::OVERRIDE_COLOR ? command.color : defaultColor;
        if (command.flags & PanelRenderCommandFlag::OVERRIDE_ALPHA) {
            color.a = command.color.a;
        }

        switch (command.type) {
            case PanelRenderCommandType::RECT: {
                PushSprite((uint32)SpriteId::PIXEL, command.position, command.commandRect.size, PANEL_ITEM_DEPTH,
                           command.anchor, color, screenSize, spriteRenderState);
            } break;
            case PanelRenderCommandType::TEXT: {
                PushText(*command.commandText.fontFace, command.commandText.text, command.position, PANEL_TEXT_DEPTH,
                         command.anchor.x, color, screenSize, textRenderState);
            } break;
        }
    }
}
