#pragma once

#include <windows.h>

#include <vector>

struct WidgetAlignmentGuides {
    bool vertical = false;
    int verticalX = 0;
    int verticalTop = 0;
    int verticalBottom = 0;
    bool horizontal = false;
    int horizontalY = 0;
    int horizontalLeft = 0;
    int horizontalRight = 0;
};

WidgetAlignmentGuides SnapMovingWidget(
    RECT& screenRect,
    int dpi,
    const std::vector<RECT>& otherScreenRects);

WidgetAlignmentGuides SnapSizingWidget(
    RECT& screenRect,
    WPARAM sizingEdge,
    int dpi,
    const std::vector<RECT>& otherScreenRects);

class WidgetAlignmentGuideOverlay {
public:
    static void Update(HINSTANCE instance, const WidgetAlignmentGuides& guides);
    static void Hide();
};
