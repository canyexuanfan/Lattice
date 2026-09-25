#pragma once

#include <Windows.h>
#include <d2d1.h>

#include <string>

#include "rendering/D2DContext.h"
#include "rendering/IconCache.h"
#include "ui/IconGrid.h"

inline constexpr int kWidgetTitleHeight = 32;

struct WidgetViewRenderState {
    D2D1_SIZE_F size{};
    std::wstring title;
    bool lightTheme = false;
    bool collapsed = false;
    bool locked = false;
    bool headerHovered = false;
    int hoverHeaderButton = -1;
    int pressedHeaderButton = -1;
    bool marqueeActive = false;
    RECT marqueeRect{};
    int insertionIndex = -1;
};

void DrawWidgetViewContent(
    D2DContext& d2d,
    IconCache& iconCache,
    IconGrid& iconGrid,
    const WidgetViewRenderState& state);
