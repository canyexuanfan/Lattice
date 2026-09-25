#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <cstddef>
#include <vector>

enum class WallpaperBackdropDrawMode {
    CropTopLeft,
    StretchToDestination,
};

struct WallpaperBackdropDrawGeometry {
    D2D1_RECT_F destination{};
    D2D1_RECT_F source{};
};

struct WallpaperMonitorSlice {
    size_t monitorIndex = 0;
    RECT screenRect{};
    RECT destinationRect{};
};

bool BuildWallpaperMonitorSlices(
    const RECT& outputScreenRect,
    const std::vector<RECT>& monitorRects,
    std::vector<WallpaperMonitorSlice>& slices);

bool CalculateWallpaperBackdropDrawGeometry(
    D2D1_SIZE_F bitmapSize,
    const D2D1_RECT_F& destination,
    WallpaperBackdropDrawMode mode,
    WallpaperBackdropDrawGeometry& geometry) noexcept;

class WallpaperBackdrop {
public:
    bool Refresh(
        HWND hwnd,
        ID2D1RenderTarget* renderTarget,
        int minimumWidthPixels = 0,
        int minimumHeightPixels = 0,
        bool blur = true);
    bool Draw(
        ID2D1RenderTarget* renderTarget,
        const D2D1_RECT_F& destination,
        WallpaperBackdropDrawMode mode) const;
    bool HasBitmap() const noexcept { return bitmap_ != nullptr; }
    bool CoversPixels(int width, int height) const noexcept;
    unsigned long long Generation() const noexcept { return generation_; }
    void Release() noexcept;
#ifndef NDEBUG
    bool SetSolidForSmoke(
        ID2D1RenderTarget* renderTarget,
        int width,
        int height,
        COLORREF color);
#endif

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    SIZE pixelSize_{};
    unsigned long long generation_ = 0;
};
