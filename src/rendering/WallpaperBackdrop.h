#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <wrl/client.h>

class WallpaperBackdrop {
public:
    bool Refresh(HWND hwnd, ID2D1RenderTarget* renderTarget);
    bool Draw(ID2D1RenderTarget* renderTarget, const D2D1_RECT_F& destination) const;
    bool HasBitmap() const noexcept { return bitmap_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
};
