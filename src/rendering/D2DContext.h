#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

class D2DContext {
public:
    bool Initialize(HWND hwnd, bool premultipliedAlpha = false);
    void Resize(UINT width, UINT height);
    void BeginDraw();
    HRESULT EndDraw();

    ID2D1HwndRenderTarget* Target() const { return renderTarget_.Get(); }
    ID2D1Factory* Factory() const { return factory_.Get(); }
    IDWriteFactory* WriteFactory() const { return writeFactory_.Get(); }

    void RecreateTarget(HWND hwnd);

private:
    HWND hwnd_ = nullptr;
    bool premultipliedAlpha_ = false;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> renderTarget_;
};
