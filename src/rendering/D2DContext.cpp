#include "rendering/D2DContext.h"

bool D2DContext::Initialize(HWND hwnd, bool premultipliedAlpha) {
    hwnd_ = hwnd;
    premultipliedAlpha_ = premultipliedAlpha;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) {
        return false;
    }

    RecreateTarget(hwnd);
    return renderTarget_ != nullptr;
}

void D2DContext::RecreateTarget(HWND hwnd) {
    renderTarget_.Reset();
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT>(rect.right - rect.left),
        static_cast<UINT>(rect.bottom - rect.top));

    const FLOAT dpi = static_cast<FLOAT>(GetDpiForWindow(hwnd));
    const D2D1_PIXEL_FORMAT pixelFormat = premultipliedAlpha_
        ? D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        : D2D1::PixelFormat();
    factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            pixelFormat,
            dpi,
            dpi),
        D2D1::HwndRenderTargetProperties(hwnd, size),
        renderTarget_.GetAddressOf());
}

void D2DContext::Resize(UINT width, UINT height) {
    if (renderTarget_ != nullptr) {
        renderTarget_->Resize(D2D1::SizeU(width, height));
    }
}

void D2DContext::BeginDraw() {
    if (renderTarget_ != nullptr) {
        renderTarget_->BeginDraw();
    }
}

HRESULT D2DContext::EndDraw() {
    if (renderTarget_ == nullptr) {
        return E_FAIL;
    }
    return renderTarget_->EndDraw();
}
