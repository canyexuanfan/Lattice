#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

class DragGhostWindow {
public:
    static DragGhostWindow& Instance();

    void Prepare(
        HINSTANCE instance,
        HWND sourceWindow,
        int iconSizeDip,
        SIZE slotSizeDip);
    void Stage(
        HINSTANCE instance,
        HWND sourceWindow,
        const std::wstring& contentKey,
        HICON preparedIcon,
        const std::wstring& displayName,
        bool shortcut,
        int iconSizeDip,
        SIZE slotSizeDip);
    std::uint64_t Begin(
        HINSTANCE instance,
        HWND sourceWindow,
        const std::wstring& contentKey,
        const std::wstring& displayName,
        bool shortcut,
        int iconSizeDip,
        SIZE slotSizeDip,
        POINT grabOffsetDip,
        POINT cursorScreenPoint);
    void Update(POINT cursorScreenPoint);
    void Commit(POINT cursorScreenPoint);
    void End();
    void EndIfGeneration(std::uint64_t generation);

    bool IsVisible() const noexcept;
    bool IsCommitted() const noexcept;
    POINT TopLeftScreenPoint() const noexcept;
    std::uint64_t CurrentGeneration() const noexcept;

private:
    DragGhostWindow() = default;
    ~DragGhostWindow();

    DragGhostWindow(const DragGhostWindow&) = delete;
    DragGhostWindow& operator=(const DragGhostWindow&) = delete;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void ConfigureGeometry(HWND sourceWindow, int iconSizeDip, SIZE slotSizeDip);
    bool EnsureWindow(HINSTANCE instance);
    bool EnsureSurface();
    bool EnsureLabelFont();
    bool Present(POINT topLeftScreen, BYTE alpha);
    void Render();
    void ReleaseIcons();
    void ReleaseLabelFont();
    void ReleaseSurface();

    HWND hwnd_ = nullptr;
    HDC surfaceDc_ = nullptr;
    HBITMAP surfaceBitmap_ = nullptr;
    HGDIOBJ surfacePreviousBitmap_ = nullptr;
    HFONT labelFont_ = nullptr;
    UINT labelFontDpi_ = 0;
    SIZE surfaceSizePixels_{};
    HICON icon_ = nullptr;
    std::wstring displayName_;
    POINT grabOffsetPixels_{};
    POINT topLeftScreen_{};
    SIZE windowSizePixels_{96, 104};
    int iconSizePixels_ = 48;
    UINT dpi_ = 96;
    bool active_ = false;
    bool committed_ = false;
    bool hasPresented_ = false;
    bool shortcut_ = false;
    bool staged_ = false;
    BYTE presentedAlpha_ = 0;
    std::uint64_t generation_ = 0;
    std::wstring stagedContentKey_;
};
