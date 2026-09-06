#include "ui/DragGhostWindow.h"

#include <algorithm>

namespace {

constexpr wchar_t kDragGhostClassName[] = L"Lattice.DragGhostWindow";
constexpr COLORREF kTransparentColor = RGB(1, 2, 3);
constexpr BYTE kDraggingAlpha = 255;
constexpr BYTE kCommittedAlpha = 255;

}  // namespace

DragGhostWindow& DragGhostWindow::Instance() {
    static DragGhostWindow instance;
    return instance;
}

DragGhostWindow::~DragGhostWindow() {
    ReleaseIcons();
    ReleaseLabelFont();
    ReleaseSurface();
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void DragGhostWindow::Prepare(
    HINSTANCE instance,
    HWND sourceWindow,
    int iconSizeDip,
    SIZE slotSizeDip) {
    ConfigureGeometry(sourceWindow, iconSizeDip, slotSizeDip);
    if (!EnsureWindow(instance)) {
        return;
    }
    EnsureSurface();
    EnsureLabelFont();
}

void DragGhostWindow::Stage(
    HINSTANCE instance,
    HWND sourceWindow,
    const std::wstring& contentKey,
    HICON preparedIcon,
    const std::wstring& displayName,
    bool shortcut,
    int iconSizeDip,
    SIZE slotSizeDip) {
    End();
    ConfigureGeometry(sourceWindow, iconSizeDip, slotSizeDip);
    if (!EnsureWindow(instance)) {
        if (preparedIcon != nullptr) {
            DestroyIcon(preparedIcon);
        }
        return;
    }
    icon_ = preparedIcon;
    if (icon_ == nullptr) {
        icon_ = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    }
    displayName_ = displayName;
    shortcut_ = shortcut;
    Render();
    stagedContentKey_ = contentKey;
    staged_ = true;
}

std::uint64_t DragGhostWindow::Begin(
    HINSTANCE instance,
    HWND sourceWindow,
    const std::wstring& contentKey,
    const std::wstring& displayName,
    bool shortcut,
    int iconSizeDip,
    SIZE slotSizeDip,
    POINT grabOffsetDip,
    POINT cursorScreenPoint) {
    active_ = false;
    committed_ = false;
    hasPresented_ = false;
    if (hwnd_ != nullptr) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    const SIZE stagedSize = windowSizePixels_;
    const UINT stagedDpi = dpi_;
    ConfigureGeometry(sourceWindow, iconSizeDip, slotSizeDip);
    if (!EnsureWindow(instance)) {
        return generation_;
    }
    grabOffsetPixels_.x = MulDiv(grabOffsetDip.x, static_cast<int>(dpi_), 96);
    grabOffsetPixels_.y = MulDiv(grabOffsetDip.y, static_cast<int>(dpi_), 96);
    const bool useStagedContent =
        staged_ &&
        stagedContentKey_ == contentKey &&
        displayName_ == displayName &&
        shortcut_ == shortcut &&
        stagedDpi == dpi_ &&
        stagedSize.cx == windowSizePixels_.cx &&
        stagedSize.cy == windowSizePixels_.cy;
    if (!useStagedContent) {
        ReleaseIcons();
        displayName_ = displayName;
        icon_ = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
        shortcut_ = shortcut;
        Render();
    }
    staged_ = false;
    stagedContentKey_.clear();
    active_ = true;

    Update(cursorScreenPoint);
    return generation_;
}

void DragGhostWindow::Update(POINT cursorScreenPoint) {
    if (hwnd_ == nullptr || !active_) {
        return;
    }
    const POINT topLeft{
        cursorScreenPoint.x - grabOffsetPixels_.x,
        cursorScreenPoint.y - grabOffsetPixels_.y};
    Present(topLeft, kDraggingAlpha);
}

void DragGhostWindow::Commit(POINT cursorScreenPoint) {
    if (hwnd_ == nullptr || !active_) {
        return;
    }
    const POINT topLeft{
        cursorScreenPoint.x - grabOffsetPixels_.x,
        cursorScreenPoint.y - grabOffsetPixels_.y};
    committed_ = Present(topLeft, kCommittedAlpha);
}

void DragGhostWindow::End() {
    active_ = false;
    committed_ = false;
    hasPresented_ = false;
    if (hwnd_ != nullptr) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    displayName_.clear();
    staged_ = false;
    stagedContentKey_.clear();
    ReleaseIcons();
}

void DragGhostWindow::EndIfGeneration(std::uint64_t generation) {
    if (generation != 0 && generation == generation_) {
        End();
    }
}

bool DragGhostWindow::IsVisible() const noexcept {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

bool DragGhostWindow::IsCommitted() const noexcept {
    return active_ && committed_;
}

POINT DragGhostWindow::TopLeftScreenPoint() const noexcept {
    return topLeftScreen_;
}

std::uint64_t DragGhostWindow::CurrentGeneration() const noexcept {
    return generation_;
}

LRESULT CALLBACK DragGhostWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void DragGhostWindow::ConfigureGeometry(
    HWND sourceWindow,
    int iconSizeDip,
    SIZE slotSizeDip) {
    dpi_ = sourceWindow == nullptr ? 96 : GetDpiForWindow(sourceWindow);
    if (dpi_ == 0) {
        dpi_ = 96;
    }
    iconSizePixels_ = std::max(24, MulDiv(iconSizeDip, static_cast<int>(dpi_), 96));
    windowSizePixels_.cx = std::max(
        iconSizePixels_ + MulDiv(16, static_cast<int>(dpi_), 96),
        MulDiv(slotSizeDip.cx, static_cast<int>(dpi_), 96));
    windowSizePixels_.cy = std::max(
        iconSizePixels_ + MulDiv(42, static_cast<int>(dpi_), 96),
        MulDiv(slotSizeDip.cy, static_cast<int>(dpi_), 96));
}

bool DragGhostWindow::EnsureWindow(HINSTANCE instance) {
    if (hwnd_ != nullptr) {
        return true;
    }
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(instance, kDragGhostClassName, &existing)) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.lpszClassName = kDragGhostClassName;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
        if (RegisterClassExW(&windowClass) == 0) {
            return false;
        }
    }
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kDragGhostClassName,
        L"",
        WS_POPUP,
        0,
        0,
        windowSizePixels_.cx,
        windowSizePixels_.cy,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd_ == nullptr) {
        return false;
    }
    return true;
}

bool DragGhostWindow::EnsureLabelFont() {
    if (labelFont_ != nullptr && labelFontDpi_ == dpi_) {
        return true;
    }
    ReleaseLabelFont();
    LOGFONTW font{};
    SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(font), &font, 0);
    if (font.lfFaceName[0] == 0) {
        wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
    }
    font.lfHeight = -MulDiv(12, static_cast<int>(dpi_), 96);
    labelFont_ = CreateFontIndirectW(&font);
    if (labelFont_ != nullptr) {
        labelFontDpi_ = dpi_;
    }
    return labelFont_ != nullptr;
}

bool DragGhostWindow::EnsureSurface() {
    if (surfaceDc_ != nullptr &&
        surfaceBitmap_ != nullptr &&
        surfaceSizePixels_.cx == windowSizePixels_.cx &&
        surfaceSizePixels_.cy == windowSizePixels_.cy) {
        return true;
    }

    ReleaseSurface();
    if (windowSizePixels_.cx <= 0 || windowSizePixels_.cy <= 0) {
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return false;
    }
    HDC memoryDc = CreateCompatibleDC(screenDc);
    if (memoryDc == nullptr) {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = windowSizePixels_.cx;
    bitmapInfo.bmiHeader.biHeight = -windowSizePixels_.cy;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bitmapBits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screenDc,
        &bitmapInfo,
        DIB_RGB_COLORS,
        &bitmapBits,
        nullptr,
        0);
    ReleaseDC(nullptr, screenDc);
    if (bitmap == nullptr || bitmapBits == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        DeleteDC(memoryDc);
        return false;
    }

    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        return false;
    }

    surfaceDc_ = memoryDc;
    surfaceBitmap_ = bitmap;
    surfacePreviousBitmap_ = previousBitmap;
    surfaceSizePixels_ = windowSizePixels_;
    return true;
}

bool DragGhostWindow::Present(POINT topLeftScreen, BYTE alpha) {
    if (hwnd_ == nullptr ||
        !active_ ||
        surfaceDc_ == nullptr ||
        surfaceBitmap_ == nullptr) {
        return false;
    }
    if (hasPresented_ &&
        topLeftScreen_.x == topLeftScreen.x &&
        topLeftScreen_.y == topLeftScreen.y &&
        presentedAlpha_ == alpha &&
        IsWindowVisible(hwnd_) != FALSE) {
        return true;
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return false;
    }
    POINT sourcePoint{0, 0};
    SIZE surfaceSize = windowSizePixels_;
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = 0;
    const BOOL updated = UpdateLayeredWindow(
        hwnd_,
        screenDc,
        &topLeftScreen,
        &surfaceSize,
        surfaceDc_,
        &sourcePoint,
        kTransparentColor,
        &blend,
        ULW_COLORKEY | ULW_ALPHA);
    ReleaseDC(nullptr, screenDc);
    if (!updated) {
        return false;
    }

    topLeftScreen_ = topLeftScreen;
    presentedAlpha_ = alpha;
    hasPresented_ = true;
    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_SHOWWINDOW);
    return true;
}

void DragGhostWindow::Render() {
    if (hwnd_ == nullptr || !EnsureSurface()) {
        return;
    }
    HDC dc = surfaceDc_;
    RECT client{0, 0, windowSizePixels_.cx, windowSizePixels_.cy};
    HBRUSH transparentBrush = CreateSolidBrush(kTransparentColor);
    FillRect(dc, &client, transparentBrush);
    DeleteObject(transparentBrush);

    const int iconLeft = (client.right - iconSizePixels_) / 2;
    const int iconTop = MulDiv(6, static_cast<int>(dpi_), 96);
    if (icon_ != nullptr) {
        DrawIconEx(dc, iconLeft, iconTop, icon_, iconSizePixels_, iconSizePixels_, 0, nullptr, DI_NORMAL);
    }
    if (shortcut_) {
        const int overlaySize = std::max(12, MulDiv(16, static_cast<int>(dpi_), 96));
        const int overlayLeft = iconLeft - MulDiv(1, static_cast<int>(dpi_), 96);
        const int overlayTop =
            iconTop + iconSizePixels_ - overlaySize + MulDiv(1, static_cast<int>(dpi_), 96);
        const auto scaled = [overlaySize](int value) {
            return MulDiv(value, overlaySize, 16);
        };
        HBRUSH darkBrush = CreateSolidBrush(RGB(36, 49, 58));
        HBRUSH lightBrush = CreateSolidBrush(RGB(248, 250, 252));
        RECT part{
            overlayLeft + scaled(2), overlayTop + scaled(9),
            overlayLeft + scaled(11), overlayTop + scaled(14)};
        FillRect(dc, &part, darkBrush);
        part = RECT{
            overlayLeft + scaled(9), overlayTop + scaled(3),
            overlayLeft + scaled(14), overlayTop + scaled(12)};
        FillRect(dc, &part, darkBrush);
        part = RECT{
            overlayLeft + scaled(4), overlayTop + scaled(10),
            overlayLeft + scaled(11), overlayTop + scaled(12)};
        FillRect(dc, &part, lightBrush);
        part = RECT{
            overlayLeft + scaled(10), overlayTop + scaled(5),
            overlayLeft + scaled(12), overlayTop + scaled(11)};
        FillRect(dc, &part, lightBrush);
        part = RECT{
            overlayLeft + scaled(7), overlayTop + scaled(5),
            overlayLeft + scaled(13), overlayTop + scaled(7)};
        FillRect(dc, &part, lightBrush);
        DeleteObject(lightBrush);
        DeleteObject(darkBrush);
    }

    EnsureLabelFont();
    HGDIOBJ oldFont = labelFont_ == nullptr
        ? nullptr
        : SelectObject(dc, labelFont_);
    SetBkMode(dc, TRANSPARENT);
    RECT labelRect{
        0,
        iconTop + iconSizePixels_ + MulDiv(4, static_cast<int>(dpi_), 96),
        client.right,
        client.bottom};
    RECT shadowRect = labelRect;
    OffsetRect(&shadowRect, 1, 1);
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(
        dc,
        displayName_.c_str(),
        -1,
        &shadowRect,
        DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(
        dc,
        displayName_.c_str(),
        -1,
        &labelRect,
        DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (oldFont != nullptr && oldFont != HGDI_ERROR) {
        SelectObject(dc, oldFont);
    }
    GdiFlush();
}

void DragGhostWindow::ReleaseSurface() {
    if (surfaceDc_ != nullptr && surfacePreviousBitmap_ != nullptr) {
        SelectObject(surfaceDc_, surfacePreviousBitmap_);
    }
    surfacePreviousBitmap_ = nullptr;
    if (surfaceBitmap_ != nullptr) {
        DeleteObject(surfaceBitmap_);
        surfaceBitmap_ = nullptr;
    }
    if (surfaceDc_ != nullptr) {
        DeleteDC(surfaceDc_);
        surfaceDc_ = nullptr;
    }
    surfaceSizePixels_ = {};
}

void DragGhostWindow::ReleaseIcons() {
    if (icon_ != nullptr) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
    shortcut_ = false;
}

void DragGhostWindow::ReleaseLabelFont() {
    if (labelFont_ != nullptr) {
        DeleteObject(labelFont_);
        labelFont_ = nullptr;
    }
    labelFontDpi_ = 0;
}
