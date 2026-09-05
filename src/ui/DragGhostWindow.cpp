#include "ui/DragGhostWindow.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

namespace {

constexpr wchar_t kDragGhostClassName[] = L"Lattice.DragGhostWindow";
constexpr COLORREF kTransparentColor = RGB(1, 2, 3);
constexpr BYTE kDraggingAlpha = 255;
constexpr BYTE kCommittedAlpha = 255;

HICON LoadShellIcon(const std::wstring& path) {
    SHFILEINFOW fileInfo{};
    if (SHGetFileInfoW(
            path.c_str(),
            0,
            &fileInfo,
            sizeof(fileInfo),
            SHGFI_ICON | SHGFI_SYSICONINDEX) == 0) {
        return nullptr;
    }

    const int imageIndex = fileInfo.iIcon & 0x00FFFFFF;
    Microsoft::WRL::ComPtr<IImageList> imageList;
    HICON icon = nullptr;
    if (SUCCEEDED(SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(imageList.GetAddressOf()))) &&
        imageList != nullptr) {
        imageList->GetIcon(imageIndex, ILD_TRANSPARENT, &icon);
    }
    if (icon == nullptr && fileInfo.hIcon != nullptr) {
        icon = fileInfo.hIcon;
        fileInfo.hIcon = nullptr;
    }
    if (fileInfo.hIcon != nullptr) {
        DestroyIcon(fileInfo.hIcon);
    }
    return icon;
}

HICON LoadShortcutOverlay() {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON | SHGSI_SMALLICON, &info))) {
        return nullptr;
    }
    return info.hIcon;
}

}  // namespace

DragGhostWindow& DragGhostWindow::Instance() {
    static DragGhostWindow instance;
    return instance;
}

DragGhostWindow::~DragGhostWindow() {
    ReleaseIcons();
    ReleaseSurface();
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

std::uint64_t DragGhostWindow::Begin(
    HINSTANCE instance,
    HWND sourceWindow,
    const std::wstring& path,
    const std::wstring& displayName,
    bool shortcut,
    int iconSizeDip,
    SIZE slotSizeDip,
    POINT grabOffsetDip,
    POINT cursorScreenPoint) {
    End();
    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    if (!EnsureWindow(instance)) {
        return generation_;
    }
    committed_ = false;

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
    grabOffsetPixels_.x = MulDiv(grabOffsetDip.x, static_cast<int>(dpi_), 96);
    grabOffsetPixels_.y = MulDiv(grabOffsetDip.y, static_cast<int>(dpi_), 96);
    displayName_ = displayName;
    icon_ = LoadShellIcon(path);
    shortcutOverlay_ = shortcut ? LoadShortcutOverlay() : nullptr;
    active_ = true;

    Render();
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
    if (hwnd_ != nullptr) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    displayName_.clear();
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
    if (shortcutOverlay_ != nullptr) {
        const int overlaySize = std::max(12, MulDiv(16, static_cast<int>(dpi_), 96));
        DrawIconEx(
            dc,
            iconLeft - MulDiv(1, static_cast<int>(dpi_), 96),
            iconTop + iconSizePixels_ - overlaySize + MulDiv(1, static_cast<int>(dpi_), 96),
            shortcutOverlay_,
            overlaySize,
            overlaySize,
            0,
            nullptr,
            DI_NORMAL);
    }

    LOGFONTW font{};
    SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(font), &font, 0);
    if (font.lfFaceName[0] == 0) {
        wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
    }
    font.lfHeight = -MulDiv(12, static_cast<int>(dpi_), 96);
    HFONT labelFont = CreateFontIndirectW(&font);
    HGDIOBJ oldFont = SelectObject(dc, labelFont);
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
    SelectObject(dc, oldFont);
    DeleteObject(labelFont);
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
    if (shortcutOverlay_ != nullptr) {
        DestroyIcon(shortcutOverlay_);
        shortcutOverlay_ = nullptr;
    }
}
