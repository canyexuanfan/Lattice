#pragma once

#include <Windows.h>

#include <shellapi.h>

#include <string>

constexpr UINT kTrayIconMessage = WM_APP + 10;

class TrayIcon {
public:
    ~TrayIcon();

    bool Initialize(HWND owner, HINSTANCE instance);
    bool ShowNotification(
        const std::wstring& title,
        const std::wstring& message,
        DWORD flags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME);
    void Remove();
    bool IsVisible() const noexcept { return visible_; }

private:
    HWND owner_ = nullptr;
    UINT id_ = 1;
    bool visible_ = false;
};
