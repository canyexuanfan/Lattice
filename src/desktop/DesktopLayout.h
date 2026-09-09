#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

struct DesktopPosition {
    std::wstring path;
    POINT point{};
};

struct DesktopViewItem {
    std::wstring path;
    std::wstring displayName;
    POINT viewPoint{};
    POINT screenPoint{};
    int viewIndex = -1;
    int systemImageIndex = -1;
    int overlayIndex = 0;
    std::vector<BYTE> shellChildPidl;
};

struct DesktopViewSnapshot {
    HWND desktopHost = nullptr;
    HWND shellViewWindow = nullptr;
    HWND listViewWindow = nullptr;
    RECT screenRect{};
    DWORD viewFlags = 0;
    int viewIconSize = 48;
    std::vector<DesktopViewItem> items;
};

class DesktopLayout {
public:
    bool CaptureViewSnapshot(
        DesktopViewSnapshot& snapshot,
        std::wstring& errorMessage) const;
    bool CaptureViewFlags(DWORD& flags, std::wstring& errorMessage) const;
    bool CaptureAllPositions(std::vector<DesktopPosition>& positions, std::wstring& errorMessage) const;
    bool CapturePosition(const std::wstring& path, POINT& point, std::wstring& errorMessage) const;
    bool CaptureScreenPosition(const std::wstring& path, POINT& point, std::wstring& errorMessage) const;
    bool RestorePosition(
        const std::wstring& path,
        POINT viewPoint,
        std::wstring& errorMessage) const;
    bool RestoreScreenPositionOnce(
        const std::wstring& path,
        POINT screenPoint,
        POINT& restoredPoint,
        std::wstring& errorMessage) const;
    bool RestoreScreenPosition(
        const std::wstring& path,
        POINT screenPoint,
        POINT& restoredPoint,
        std::wstring& errorMessage,
        const std::function<void()>& onVisiblePositioned = {},
        const std::function<bool()>& cancellationRequested = {}) const;
    bool RestorePositions(const std::vector<DesktopPosition>& positions, std::wstring& errorMessage) const;
    bool RestorePositions(
        const std::vector<DesktopPosition>& positions,
        const std::vector<std::wstring>& requiredPaths,
        std::wstring& errorMessage,
        bool notifyRequiredPaths = true) const;
    bool PositionScreenItemsOnce(
        const std::vector<DesktopPosition>& screenPositions,
        std::vector<DesktopPosition>& confirmedScreenPositions,
        std::wstring& errorMessage) const;
};
