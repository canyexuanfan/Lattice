#pragma once

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

struct IconAsyncState;

enum class IconPlaceholderKind : std::uint8_t {
    File,
    Folder,
};

class IconCache {
public:
    IconCache();
    ~IconCache();
    ID2D1Bitmap* GetIcon(
        ID2D1RenderTarget* target,
        const std::wstring& path,
        const std::wstring& displayName = L"",
        IconPlaceholderKind placeholderKind = IconPlaceholderKind::File,
        bool* usedPlaceholder = nullptr);
    bool IsIconReady(ID2D1RenderTarget* target, const std::wstring& path);
    HICON CopyReadyIconForDrag(const std::wstring& path);
    void Preload(const std::wstring& path);
    void Alias(const std::wstring& sourcePath, const std::wstring& destinationPath);
    void Clear();
    size_t Size() const noexcept;
    void SetCapacity(size_t capacity);
    void SetInvalidateCallback(std::function<void()> callback);

private:
    void EnsureTargetResources(ID2D1RenderTarget* target);
    void ProcessCompleted(ID2D1RenderTarget* target);
    void Enqueue(const std::wstring& path);
    ID2D1Bitmap* GetPlaceholder(
        ID2D1RenderTarget* target,
        IconPlaceholderKind placeholderKind);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> CreatePixelBitmap(
        ID2D1RenderTarget* target,
        const std::uint32_t* pixels,
        UINT width,
        UINT height);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> LoadIconBitmap(ID2D1RenderTarget* target, HICON icon);

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    mutable std::mutex cacheMutex_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> resourceTarget_;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> cache_;
    std::unordered_map<std::wstring, HICON> dragIconCache_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> filePlaceholder_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> folderPlaceholder_;
    size_t capacity_ = 256;
    std::shared_ptr<IconAsyncState> asyncState_;
};
