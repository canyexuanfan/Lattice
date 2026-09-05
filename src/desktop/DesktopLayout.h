#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

struct DesktopPosition {
    std::wstring path;
    POINT point{};
};

class DesktopLayout {
public:
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
};
