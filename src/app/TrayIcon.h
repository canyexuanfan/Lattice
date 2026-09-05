#pragma once

#include <Windows.h>

constexpr UINT kTrayIconMessage = WM_APP + 10;

class TrayIcon {
public:
    ~TrayIcon();

    bool Initialize(HWND owner, HINSTANCE instance);
    void Remove();
    bool IsVisible() const noexcept { return visible_; }

private:
    HWND owner_ = nullptr;
    UINT id_ = 1;
    bool visible_ = false;
};
