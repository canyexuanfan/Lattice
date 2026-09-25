#include "ui/WidgetAlignment.h"

#include <algorithm>
#include <cstdlib>

namespace {

constexpr wchar_t kGuideClass[] = L"Lattice.AlignmentGuide";

HWND& VerticalWindow() {
    static HWND window = nullptr;
    return window;
}

HWND& HorizontalWindow() {
    static HWND window = nullptr;
    return window;
}

void EnsureGuideWindows(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(instance, kGuideClass, &existing)) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.lpszClassName = kGuideClass;
        windowClass.hbrBackground = CreateSolidBrush(RGB(63, 211, 241));
        RegisterClassExW(&windowClass);
    }
    if (VerticalWindow() == nullptr) {
        VerticalWindow() = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                WS_EX_NOACTIVATE,
            kGuideClass, L"", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, instance, nullptr);
        if (VerticalWindow() != nullptr) {
            SetLayeredWindowAttributes(VerticalWindow(), 0, 220, LWA_ALPHA);
        }
    }
    if (HorizontalWindow() == nullptr) {
        HorizontalWindow() = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                WS_EX_NOACTIVATE,
            kGuideClass, L"", WS_POPUP, 0, 0, 1, 1,
            nullptr, nullptr, instance, nullptr);
        if (HorizontalWindow() != nullptr) {
            SetLayeredWindowAttributes(HorizontalWindow(), 0, 220, LWA_ALPHA);
        }
    }
}

void PositionLine(HWND window, bool show, int x, int y,
                  int width, int height) {
    if (window == nullptr) return;
    if (!show) {
        ShowWindow(window, SW_HIDE);
        return;
    }
    SetWindowPos(window, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

} // namespace

void WidgetAlignmentGuideOverlay::Update(
    HINSTANCE instance, const WidgetAlignmentGuides& guides) {
    if (!guides.vertical && !guides.horizontal) {
        Hide();
        return;
    }
    EnsureGuideWindows(instance);
    PositionLine(VerticalWindow(), guides.vertical,
                 guides.verticalX - 1, guides.verticalTop, 2,
                 std::max(1, guides.verticalBottom - guides.verticalTop));
    PositionLine(HorizontalWindow(), guides.horizontal,
                 guides.horizontalLeft, guides.horizontalY - 1,
                 std::max(1, guides.horizontalRight - guides.horizontalLeft), 2);
}

void WidgetAlignmentGuideOverlay::Hide() {
    if (VerticalWindow() != nullptr) ShowWindow(VerticalWindow(), SW_HIDE);
    if (HorizontalWindow() != nullptr) ShowWindow(HorizontalWindow(), SW_HIDE);
}

WidgetAlignmentGuides SnapMovingWidget(
    RECT& movingRect, int dpi,
    const std::vector<RECT>& otherScreenRects) {
    const int width = movingRect.right - movingRect.left;
    const int height = movingRect.bottom - movingRect.top;
    const int alignmentThreshold = MulDiv(4, std::max(96, dpi), 96);
    const int adjacencyThreshold = MulDiv(2, std::max(96, dpi), 96);
    int bestX = movingRect.left;
    int bestY = movingRect.top;
    int bestDx = alignmentThreshold + 1;
    int bestDy = alignmentThreshold + 1;
    WidgetAlignmentGuides guides;

    const auto considerX = [&](int candidate, int guideX, int guideTop,
                               int guideBottom, int threshold) {
        const int delta = std::abs(candidate - movingRect.left);
        if (delta < bestDx && delta <= threshold) {
            bestDx = delta;
            bestX = candidate;
            guides.verticalX = guideX;
            guides.verticalTop = guideTop;
            guides.verticalBottom = guideBottom;
        }
    };
    const auto considerY = [&](int candidate, int guideY, int guideLeft,
                               int guideRight, int threshold) {
        const int delta = std::abs(candidate - movingRect.top);
        if (delta < bestDy && delta <= threshold) {
            bestDy = delta;
            bestY = candidate;
            guides.horizontalY = guideY;
            guides.horizontalLeft = guideLeft;
            guides.horizontalRight = guideRight;
        }
    };

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(MonitorFromRect(&movingRect,
                                       MONITOR_DEFAULTTONEAREST),
                        &monitorInfo)) {
        const RECT& work = monitorInfo.rcWork;
        considerX(work.left, work.left, work.top, work.bottom,
                  alignmentThreshold);
        considerX(work.right - width, work.right, work.top, work.bottom,
                  alignmentThreshold);
        considerY(work.top, work.top, work.left, work.right,
                  alignmentThreshold);
        considerY(work.bottom - height, work.bottom, work.left, work.right,
                  alignmentThreshold);
    }

    for (const RECT& other : otherScreenRects) {
        const int guideTop = std::min(movingRect.top, other.top);
        const int guideBottom = std::max(movingRect.bottom, other.bottom);
        const int guideLeft = std::min(movingRect.left, other.left);
        const int guideRight = std::max(movingRect.right, other.right);
        considerX(other.left, other.left, guideTop, guideBottom,
                  alignmentThreshold);
        considerX(other.right - width, other.right, guideTop, guideBottom,
                  alignmentThreshold);
        considerX(other.left - width, other.left, guideTop, guideBottom,
                  adjacencyThreshold);
        considerX(other.right, other.right, guideTop, guideBottom,
                  adjacencyThreshold);
        considerX((other.left + other.right - width) / 2,
                  (other.left + other.right) / 2, guideTop, guideBottom,
                  alignmentThreshold);
        considerY(other.top, other.top, guideLeft, guideRight,
                  alignmentThreshold);
        considerY(other.bottom - height, other.bottom, guideLeft, guideRight,
                  alignmentThreshold);
        considerY(other.top - height, other.top, guideLeft, guideRight,
                  adjacencyThreshold);
        considerY(other.bottom, other.bottom, guideLeft, guideRight,
                  adjacencyThreshold);
        considerY((other.top + other.bottom - height) / 2,
                  (other.top + other.bottom) / 2, guideLeft, guideRight,
                  alignmentThreshold);
    }

    movingRect.left = bestX;
    movingRect.top = bestY;
    movingRect.right = bestX + width;
    movingRect.bottom = bestY + height;
    guides.vertical = bestDx <= alignmentThreshold;
    guides.horizontal = bestDy <= alignmentThreshold;
    return guides;
}

WidgetAlignmentGuides SnapSizingWidget(
    RECT& sizingRect, WPARAM sizingEdge, int dpi,
    const std::vector<RECT>& otherScreenRects) {
    const int threshold = MulDiv(4, std::max(96, dpi), 96);
    const int minimumWidth = MulDiv(260, std::max(96, dpi), 96);
    const int minimumHeight = MulDiv(120, std::max(96, dpi), 96);
    const bool resizeLeft = sizingEdge == WMSZ_LEFT ||
        sizingEdge == WMSZ_TOPLEFT || sizingEdge == WMSZ_BOTTOMLEFT;
    const bool resizeRight = sizingEdge == WMSZ_RIGHT ||
        sizingEdge == WMSZ_TOPRIGHT || sizingEdge == WMSZ_BOTTOMRIGHT;
    const bool resizeTop = sizingEdge == WMSZ_TOP ||
        sizingEdge == WMSZ_TOPLEFT || sizingEdge == WMSZ_TOPRIGHT;
    const bool resizeBottom = sizingEdge == WMSZ_BOTTOM ||
        sizingEdge == WMSZ_BOTTOMLEFT || sizingEdge == WMSZ_BOTTOMRIGHT;
    int bestDx = threshold + 1;
    int bestDy = threshold + 1;
    int bestEdgeX = 0;
    int bestEdgeY = 0;
    WidgetAlignmentGuides guides;

    const auto considerX = [&](int candidate, int guideTop,
                               int guideBottom) {
        const int current = resizeLeft ? sizingRect.left : sizingRect.right;
        const int delta = std::abs(candidate - current);
        const bool keepsMinimum = resizeLeft
            ? sizingRect.right - candidate >= minimumWidth
            : candidate - sizingRect.left >= minimumWidth;
        if (keepsMinimum && delta < bestDx && delta <= threshold) {
            bestDx = delta;
            bestEdgeX = candidate;
            guides.verticalTop = guideTop;
            guides.verticalBottom = guideBottom;
        }
    };
    const auto considerY = [&](int candidate, int guideLeft,
                               int guideRight) {
        const int current = resizeTop ? sizingRect.top : sizingRect.bottom;
        const int delta = std::abs(candidate - current);
        const bool keepsMinimum = resizeTop
            ? sizingRect.bottom - candidate >= minimumHeight
            : candidate - sizingRect.top >= minimumHeight;
        if (keepsMinimum && delta < bestDy && delta <= threshold) {
            bestDy = delta;
            bestEdgeY = candidate;
            guides.horizontalLeft = guideLeft;
            guides.horizontalRight = guideRight;
        }
    };

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(MonitorFromRect(&sizingRect,
                                       MONITOR_DEFAULTTONEAREST),
                        &monitorInfo)) {
        const RECT& work = monitorInfo.rcWork;
        if (resizeLeft || resizeRight) {
            considerX(work.left, work.top, work.bottom);
            considerX(work.right, work.top, work.bottom);
        }
        if (resizeTop || resizeBottom) {
            considerY(work.top, work.left, work.right);
            considerY(work.bottom, work.left, work.right);
        }
    }

    for (const RECT& other : otherScreenRects) {
        if (resizeLeft || resizeRight) {
            const int guideTop = std::min(sizingRect.top, other.top);
            const int guideBottom = std::max(sizingRect.bottom, other.bottom);
            considerX(other.left, guideTop, guideBottom);
            considerX(other.right, guideTop, guideBottom);
        }
        if (resizeTop || resizeBottom) {
            const int guideLeft = std::min(sizingRect.left, other.left);
            const int guideRight = std::max(sizingRect.right, other.right);
            considerY(other.top, guideLeft, guideRight);
            considerY(other.bottom, guideLeft, guideRight);
        }
    }

    if (bestDx <= threshold) {
        if (resizeLeft) sizingRect.left = bestEdgeX;
        else if (resizeRight) sizingRect.right = bestEdgeX;
        guides.vertical = true;
        guides.verticalX = bestEdgeX;
    }
    if (bestDy <= threshold) {
        if (resizeTop) sizingRect.top = bestEdgeY;
        else if (resizeBottom) sizingRect.bottom = bestEdgeY;
        guides.horizontal = true;
        guides.horizontalY = bestEdgeY;
    }
    return guides;
}
