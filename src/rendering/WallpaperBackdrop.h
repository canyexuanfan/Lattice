#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <functional>
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
#ifndef NDEBUG
    struct RefreshProfileForTesting {
        bool enabled = false, complete = false, succeeded = false, blur = false;
        int outputWidth = 0, outputHeight = 0;
        DWORD threadId = 0;
        std::uint64_t generation = 0, sampledCapacityBytes = 0, outputCapacityBytes = 0;
        std::uint64_t stageMicroseconds[6]{};
    };
#endif
    struct PrepareInput {
        POINT origin{};
        SIZE size{};
        UINT dpi = 96;
        bool blur = true;
    };
    struct PreparedPixels {
        PrepareInput input;
        std::vector<BYTE> pixels;
#ifndef NDEBUG
        RefreshProfileForTesting profile;
#endif
    };
    static bool CapturePrepareInput(HWND hwnd, int minimumWidthPixels,
        int minimumHeightPixels, bool blur, PrepareInput& input);
    static bool PreparePixels(const PrepareInput& input, PreparedPixels& prepared,
        bool shareDecodedWave = true,
        const std::function<bool()>& cancelled = {});
    bool CommitPreparedPixels(ID2D1RenderTarget* renderTarget,
        const PreparedPixels& prepared);
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
    const RefreshProfileForTesting& LastRefreshProfileForTesting() const noexcept {
        return refreshProfile_;
    }
    std::uint64_t PixelBytesForTesting() const noexcept {
        return bitmap_ == nullptr ? 0 :
            static_cast<std::uint64_t>(pixelSize_.cx) *
            static_cast<std::uint64_t>(pixelSize_.cy) * 4;
    }
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
#ifndef NDEBUG
    RefreshProfileForTesting refreshProfile_;
#endif
};
