#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <wrl/client.h>

enum class WallpaperBackdropDrawMode {
    CropTopLeft,
    StretchToDestination,
};

struct WallpaperBackdropDrawGeometry {
    D2D1_RECT_F destination{};
    D2D1_RECT_F source{};
};

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

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    SIZE pixelSize_{};
};
