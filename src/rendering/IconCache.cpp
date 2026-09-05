#include "rendering/IconCache.h"

#include <Windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

struct CompletedIcon {
    CompletedIcon() = default;
    CompletedIcon(HICON value, std::uint64_t valueGeneration)
        : icon(value), generation(valueGeneration) {
    }
    ~CompletedIcon() {
        Reset();
    }

    CompletedIcon(const CompletedIcon&) = delete;
    CompletedIcon& operator=(const CompletedIcon&) = delete;

    CompletedIcon(CompletedIcon&& other) noexcept
        : icon(other.icon), generation(other.generation) {
        other.icon = nullptr;
    }

    CompletedIcon& operator=(CompletedIcon&& other) noexcept {
        if (this != &other) {
            Reset();
            icon = other.icon;
            generation = other.generation;
            other.icon = nullptr;
        }
        return *this;
    }

    void Reset() noexcept {
        if (icon != nullptr) {
            DestroyIcon(icon);
            icon = nullptr;
        }
    }

    HICON icon = nullptr;
    std::uint64_t generation = 0;
};

struct PendingIconRequest {
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::wstring loadPath;
    std::wstring resultPath;
};

struct FailedIconLoad {
    std::uint64_t generation = 0;
    ULONGLONG retryAfterTick = 0;
};

struct IconAsyncState {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, PendingIconRequest> pendingById;
    std::unordered_map<std::wstring, std::uint64_t> pendingIdByPath;
    std::unordered_map<std::wstring, CompletedIcon> completed;
    std::unordered_map<std::wstring, FailedIconLoad> failedByPath;
    std::function<void()> invalidateCallback;
    size_t asyncCapacity = 256;
    std::uint64_t nextRequestId = 1;
    std::uint64_t generation = 1;
    bool invalidatePosted = false;
    bool active = true;
};

namespace {

constexpr ULONGLONG kFailedIconRetryDelayMilliseconds = 2000;

void RecordFailedIconLoadLocked(
    IconAsyncState& state,
    const std::wstring& path,
    std::uint64_t generation) {
    if (!state.active ||
        path.empty() ||
        generation != state.generation) {
        return;
    }
    if (state.failedByPath.find(path) == state.failedByPath.end()) {
        while (state.failedByPath.size() >= state.asyncCapacity &&
               !state.failedByPath.empty()) {
            state.failedByPath.erase(state.failedByPath.begin());
        }
    }
    state.failedByPath.insert_or_assign(
        path,
        FailedIconLoad{
            generation,
            GetTickCount64() + kFailedIconRetryDelayMilliseconds});
}

bool HasActiveFailedIconBackoffLocked(
    IconAsyncState& state,
    const std::wstring& path) {
    const auto failure = state.failedByPath.find(path);
    if (failure == state.failedByPath.end()) {
        return false;
    }
    if (failure->second.generation != state.generation ||
        GetTickCount64() >= failure->second.retryAfterTick) {
        state.failedByPath.erase(failure);
        return false;
    }
    return true;
}

HICON LoadShellIcon(const std::wstring& path) {
    SHFILEINFOW fileInfo{};
    constexpr UINT flags = SHGFI_ICON | SHGFI_SYSICONINDEX;
    const DWORD_PTR infoResult = SHGetFileInfoW(
        path.c_str(),
        0,
        &fileInfo,
        sizeof(fileInfo),
        flags);
    if (infoResult == 0) {
        return nullptr;
    }

    const int imageIndex = fileInfo.iIcon & 0x00FFFFFF;
    Microsoft::WRL::ComPtr<IImageList> imageList;
    HICON shellIcon = nullptr;
    if (SUCCEEDED(SHGetImageList(
            SHIL_EXTRALARGE,
            IID_PPV_ARGS(imageList.GetAddressOf()))) &&
        imageList != nullptr) {
        imageList->GetIcon(imageIndex, ILD_TRANSPARENT, &shellIcon);
    }

    if (shellIcon != nullptr) {
        if (fileInfo.hIcon != nullptr) {
            DestroyIcon(fileInfo.hIcon);
        }
        return shellIcon;
    }
    return fileInfo.hIcon;
}

HICON LoadShellIconWithFallback(const std::wstring& path) {
    HICON icon = LoadShellIcon(path);
    if (icon != nullptr) {
        return icon;
    }

    SHFILEINFOW fallbackInfo{};
    const DWORD_PTR fallbackResult = SHGetFileInfoW(
        path.c_str(),
        0,
        &fallbackInfo,
        sizeof(fallbackInfo),
        SHGFI_ICON | SHGFI_LARGEICON | SHGFI_SHELLICONSIZE);
    return fallbackResult != 0 ? fallbackInfo.hIcon : nullptr;
}

void FillRect(
    std::vector<std::uint32_t>& pixels,
    UINT width,
    UINT height,
    UINT left,
    UINT top,
    UINT right,
    UINT bottom,
    std::uint32_t color) {
    left = std::min(left, width);
    right = std::min(right, width);
    top = std::min(top, height);
    bottom = std::min(bottom, height);
    for (UINT y = top; y < bottom; ++y) {
        for (UINT x = left; x < right; ++x) {
            pixels[static_cast<size_t>(y) * width + x] = color;
        }
    }
}

std::vector<std::uint32_t> BuildFilePlaceholderPixels() {
    constexpr UINT size = 48;
    std::vector<std::uint32_t> pixels(size * size, 0);
    FillRect(pixels, size, size, 9, 4, 38, 44, 0xFF7B8792);
    FillRect(pixels, size, size, 11, 6, 36, 42, 0xFFE9EDF0);
    FillRect(pixels, size, size, 28, 6, 36, 14, 0xFFB9C2CA);
    FillRect(pixels, size, size, 15, 21, 32, 23, 0xFFA3ADB6);
    FillRect(pixels, size, size, 15, 27, 32, 29, 0xFFA3ADB6);
    FillRect(pixels, size, size, 15, 33, 28, 35, 0xFFA3ADB6);
    return pixels;
}

std::vector<std::uint32_t> BuildFolderPlaceholderPixels() {
    constexpr UINT size = 48;
    std::vector<std::uint32_t> pixels(size * size, 0);
    FillRect(pixels, size, size, 7, 8, 26, 18, 0xFF936A20);
    FillRect(pixels, size, size, 9, 10, 24, 18, 0xFFF0C65B);
    FillRect(pixels, size, size, 5, 14, 43, 41, 0xFF936A20);
    FillRect(pixels, size, size, 7, 17, 41, 39, 0xFFE4B146);
    FillRect(pixels, size, size, 7, 17, 41, 21, 0xFFF4CE72);
    return pixels;
}

std::vector<std::uint32_t> BuildShortcutOverlayPixels() {
    constexpr UINT size = 16;
    std::vector<std::uint32_t> pixels(size * size, 0);
    FillRect(pixels, size, size, 2, 9, 11, 14, 0xFF24313A);
    FillRect(pixels, size, size, 9, 3, 14, 12, 0xFF24313A);
    FillRect(pixels, size, size, 4, 10, 11, 12, 0xFFF8FAFC);
    FillRect(pixels, size, size, 10, 5, 12, 11, 0xFFF8FAFC);
    FillRect(pixels, size, size, 7, 5, 13, 7, 0xFFF8FAFC);
    return pixels;
}

void DestroyCompletedIcons(std::unordered_map<std::wstring, CompletedIcon>& icons) {
    icons.clear();
}

class SharedIconLoader {
public:
    static SharedIconLoader& Instance() {
        static SharedIconLoader loader;
        return loader;
    }

    bool Submit(
        std::uint64_t requestId,
        const std::shared_ptr<IconAsyncState>& state) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (requests_.size() >= kRequestCapacity) {
                std::queue<Request> liveRequests;
                while (!requests_.empty()) {
                    Request request = std::move(requests_.front());
                    requests_.pop();
                    const std::shared_ptr<IconAsyncState> requestState =
                        request.state.lock();
                    if (requestState == nullptr) {
                        continue;
                    }
                    std::lock_guard<std::mutex> stateLock(
                        requestState->mutex);
                    const auto pending =
                        requestState->pendingById.find(request.id);
                    if (requestState->active &&
                        pending != requestState->pendingById.end() &&
                        pending->second.generation ==
                            requestState->generation) {
                        liveRequests.push(std::move(request));
                    }
                }
                requests_.swap(liveRequests);
                if (requests_.size() >= kRequestCapacity) {
                    return false;
                }
            }
            requests_.push(Request{requestId, state});
        }
        condition_.notify_one();
        return true;
    }

private:
    struct Request {
        std::uint64_t id = 0;
        std::weak_ptr<IconAsyncState> state;
    };

    static constexpr size_t kRequestCapacity = 1024;

    SharedIconLoader()
        : worker_(&SharedIconLoader::WorkerLoop, this) {
    }

    ~SharedIconLoader() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    SharedIconLoader(const SharedIconLoader&) = delete;
    SharedIconLoader& operator=(const SharedIconLoader&) = delete;

    void WorkerLoop() {
        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const DPI_AWARENESS_CONTEXT previousDpiContext =
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&]() { return stop_ || !requests_.empty(); });
                if (stop_ && requests_.empty()) {
                    break;
                }
                request = std::move(requests_.front());
                requests_.pop();
            }

            const std::shared_ptr<IconAsyncState> state = request.state.lock();
            if (state == nullptr) {
                continue;
            }

            std::wstring loadPath;
            std::uint64_t generation = 0;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto pending = state->pendingById.find(request.id);
                if (!state->active || pending == state->pendingById.end() ||
                    pending->second.generation != state->generation) {
                    continue;
                }
                loadPath = pending->second.loadPath;
                generation = pending->second.generation;
            }

            HICON icon = LoadShellIconWithFallback(loadPath);
            if (icon == nullptr) {
                std::wstring retryPath;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    const auto pending = state->pendingById.find(request.id);
                    if (state->active && pending != state->pendingById.end() &&
                        pending->second.generation == state->generation) {
                        retryPath = pending->second.loadPath;
                    }
                }
                if (!retryPath.empty() && retryPath != loadPath) {
                    icon = LoadShellIconWithFallback(retryPath);
                }
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto pending = state->pendingById.find(request.id);
                if (pending != state->pendingById.end() &&
                    pending->second.generation == generation) {
                    const std::wstring resultPath = pending->second.resultPath;
                    const auto pathIndex = state->pendingIdByPath.find(resultPath);
                    if (pathIndex != state->pendingIdByPath.end() &&
                        pathIndex->second == request.id) {
                        state->pendingIdByPath.erase(pathIndex);
                    }
                    state->pendingById.erase(pending);
                    if (state->active && generation == state->generation) {
                        if (icon != nullptr) {
                            state->failedByPath.erase(resultPath);
                            while (state->completed.size() >= state->asyncCapacity &&
                                   !state->completed.empty()) {
                                state->completed.erase(state->completed.begin());
                            }
                            CompletedIcon completedIcon(icon, generation);
                            icon = nullptr;
                            state->completed.insert_or_assign(
                                resultPath,
                                std::move(completedIcon));
                            if (!state->invalidatePosted && state->invalidateCallback) {
                                state->invalidatePosted = true;
                                state->invalidateCallback();
                            }
                        } else {
                            RecordFailedIconLoadLocked(
                                *state,
                                resultPath,
                                generation);
                        }
                    }
                }
            }
            if (icon != nullptr) {
                DestroyIcon(icon);
            }
        }
        if (previousDpiContext != nullptr) {
            SetThreadDpiAwarenessContext(previousDpiContext);
        }
        if (SUCCEEDED(comResult)) {
            CoUninitialize();
        }
    }

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Request> requests_;
    bool stop_ = false;
};

}  // namespace

IconCache::IconCache()
    : asyncState_(std::make_shared<IconAsyncState>()) {
    CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory_));
}

IconCache::~IconCache() {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    asyncState_->active = false;
    ++asyncState_->generation;
    asyncState_->invalidateCallback = nullptr;
    asyncState_->pendingById.clear();
    asyncState_->pendingIdByPath.clear();
    asyncState_->failedByPath.clear();
    asyncState_->invalidatePosted = false;
    DestroyCompletedIcons(asyncState_->completed);
}

ID2D1Bitmap* IconCache::GetIcon(
    ID2D1RenderTarget* target,
    const std::wstring& path,
    const std::wstring& displayName,
    IconPlaceholderKind placeholderKind,
    bool* usedPlaceholder) {
    (void)displayName;
    if (usedPlaceholder != nullptr) {
        *usedPlaceholder = false;
    }
    if (target == nullptr || path.empty()) {
        return nullptr;
    }
    EnsureTargetResources(target);
    ProcessCompleted(target);
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        const auto found = cache_.find(path);
        if (found != cache_.end()) {
            return found->second.Get();
        }
    }

    Enqueue(path);
    ID2D1Bitmap* placeholder = GetPlaceholder(target, placeholderKind);
    if (placeholder != nullptr && usedPlaceholder != nullptr) {
        *usedPlaceholder = true;
    }
    return placeholder;
}

bool IconCache::IsIconReady(
    ID2D1RenderTarget* target,
    const std::wstring& path) {
    if (target == nullptr || path.empty()) {
        return false;
    }
    EnsureTargetResources(target);
    ProcessCompleted(target);
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return cache_.find(path) != cache_.end();
}

ID2D1Bitmap* IconCache::GetShortcutOverlay(ID2D1RenderTarget* target) {
    if (target == nullptr) {
        return nullptr;
    }
    EnsureTargetResources(target);
    std::lock_guard<std::mutex> lock(cacheMutex_);
    if (shortcutOverlay_ != nullptr) {
        return shortcutOverlay_.Get();
    }
    const std::vector<std::uint32_t> pixels = BuildShortcutOverlayPixels();
    shortcutOverlay_ = CreatePixelBitmap(
        target,
        pixels.data(),
        16,
        16);
    return shortcutOverlay_.Get();
}

void IconCache::Preload(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        if (cache_.find(path) != cache_.end()) {
            return;
        }
    }
    Enqueue(path);
}

void IconCache::Alias(const std::wstring& sourcePath, const std::wstring& destinationPath) {
    if (sourcePath.empty() || destinationPath.empty() || sourcePath == destinationPath) {
        return;
    }

    bool enqueueDestination = false;
    {
        std::scoped_lock lock(cacheMutex_, asyncState_->mutex);
        if (!asyncState_->active) {
            return;
        }

        cache_.erase(destinationPath);
        asyncState_->completed.erase(destinationPath);
        asyncState_->failedByPath.erase(destinationPath);
        const auto destinationPending =
            asyncState_->pendingIdByPath.find(destinationPath);
        if (destinationPending != asyncState_->pendingIdByPath.end()) {
            asyncState_->pendingById.erase(destinationPending->second);
            asyncState_->pendingIdByPath.erase(destinationPending);
        }
        asyncState_->failedByPath.erase(sourcePath);

        const auto cachedSource = cache_.find(sourcePath);
        if (cachedSource != cache_.end()) {
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap =
                std::move(cachedSource->second);
            cache_.erase(cachedSource);
            cache_[destinationPath] = std::move(bitmap);
            return;
        }

        const auto completedSource = asyncState_->completed.find(sourcePath);
        if (completedSource != asyncState_->completed.end()) {
            CompletedIcon completed = std::move(completedSource->second);
            asyncState_->completed.erase(completedSource);
            asyncState_->completed.insert_or_assign(
                destinationPath,
                std::move(completed));
            return;
        }

        const auto sourcePending = asyncState_->pendingIdByPath.find(sourcePath);
        if (sourcePending != asyncState_->pendingIdByPath.end()) {
            const std::uint64_t requestId = sourcePending->second;
            const auto request = asyncState_->pendingById.find(requestId);
            asyncState_->pendingIdByPath.erase(sourcePending);
            if (request != asyncState_->pendingById.end()) {
                request->second.loadPath = destinationPath;
                request->second.resultPath = destinationPath;
                asyncState_->pendingIdByPath[destinationPath] = requestId;
                return;
            }
        }
        enqueueDestination = true;
    }
    if (enqueueDestination) {
        Enqueue(destinationPath);
    }
}

void IconCache::Clear() {
    std::scoped_lock lock(cacheMutex_, asyncState_->mutex);
    cache_.clear();
    filePlaceholder_.Reset();
    folderPlaceholder_.Reset();
    shortcutOverlay_.Reset();
    resourceTarget_.Reset();
    ++asyncState_->generation;
    asyncState_->pendingById.clear();
    asyncState_->pendingIdByPath.clear();
    asyncState_->failedByPath.clear();
    asyncState_->invalidatePosted = false;
    DestroyCompletedIcons(asyncState_->completed);
}

size_t IconCache::Size() const noexcept {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return cache_.size();
}

void IconCache::SetCapacity(size_t capacity) {
    std::scoped_lock lock(cacheMutex_, asyncState_->mutex);
    capacity_ = std::max<size_t>(64, capacity);
    while (cache_.size() > capacity_) {
        cache_.erase(cache_.begin());
    }
    asyncState_->asyncCapacity = capacity_;
    while (asyncState_->completed.size() > asyncState_->asyncCapacity) {
        asyncState_->completed.erase(asyncState_->completed.begin());
    }
    while (asyncState_->failedByPath.size() > asyncState_->asyncCapacity) {
        asyncState_->failedByPath.erase(asyncState_->failedByPath.begin());
    }
    while (asyncState_->pendingById.size() > asyncState_->asyncCapacity) {
        const auto pending = asyncState_->pendingById.begin();
        const std::uint64_t requestId = pending->first;
        const auto pathIndex =
            asyncState_->pendingIdByPath.find(pending->second.resultPath);
        if (pathIndex != asyncState_->pendingIdByPath.end() &&
            pathIndex->second == requestId) {
            asyncState_->pendingIdByPath.erase(pathIndex);
        }
        asyncState_->pendingById.erase(pending);
    }
}

void IconCache::SetInvalidateCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    asyncState_->invalidatePosted = false;
    asyncState_->invalidateCallback = std::move(callback);
    if (asyncState_->invalidateCallback &&
        !asyncState_->completed.empty() &&
        !asyncState_->invalidatePosted) {
        asyncState_->invalidatePosted = true;
        asyncState_->invalidateCallback();
    }
}

void IconCache::Enqueue(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    std::uint64_t requestId = 0;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        if (!asyncState_->active ||
            HasActiveFailedIconBackoffLocked(*asyncState_, path)) {
            return;
        }
        if (asyncState_->completed.find(path) == asyncState_->completed.end() &&
            asyncState_->pendingIdByPath.find(path) ==
                asyncState_->pendingIdByPath.end() &&
            asyncState_->pendingById.size() + asyncState_->completed.size() <
                asyncState_->asyncCapacity) {
            requestId = asyncState_->nextRequestId++;
            if (requestId == 0) {
                requestId = asyncState_->nextRequestId++;
            }
            PendingIconRequest request;
            request.id = requestId;
            request.generation = asyncState_->generation;
            request.loadPath = path;
            request.resultPath = path;
            asyncState_->pendingById.emplace(requestId, std::move(request));
            asyncState_->pendingIdByPath[path] = requestId;
        }
    }
    if (requestId == 0 ||
        SharedIconLoader::Instance().Submit(requestId, asyncState_)) {
        return;
    }
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    asyncState_->pendingById.erase(requestId);
    for (auto pathIndex = asyncState_->pendingIdByPath.begin();
         pathIndex != asyncState_->pendingIdByPath.end();) {
        if (pathIndex->second == requestId) {
            pathIndex = asyncState_->pendingIdByPath.erase(pathIndex);
        } else {
            ++pathIndex;
        }
    }
}

void IconCache::EnsureTargetResources(ID2D1RenderTarget* target) {
    if (target == nullptr) {
        return;
    }
    std::scoped_lock lock(cacheMutex_, asyncState_->mutex);
    if (resourceTarget_.Get() == target) {
        return;
    }
    if (resourceTarget_ != nullptr) {
        cache_.clear();
        filePlaceholder_.Reset();
        folderPlaceholder_.Reset();
        shortcutOverlay_.Reset();
        ++asyncState_->generation;
        asyncState_->pendingById.clear();
        asyncState_->pendingIdByPath.clear();
        asyncState_->failedByPath.clear();
        asyncState_->invalidatePosted = false;
        DestroyCompletedIcons(asyncState_->completed);
    }
    resourceTarget_ = target;
}

ID2D1Bitmap* IconCache::GetPlaceholder(
    ID2D1RenderTarget* target,
    IconPlaceholderKind placeholderKind) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    Microsoft::WRL::ComPtr<ID2D1Bitmap>& placeholder =
        placeholderKind == IconPlaceholderKind::Folder
            ? folderPlaceholder_
            : filePlaceholder_;
    if (placeholder != nullptr) {
        return placeholder.Get();
    }
    const std::vector<std::uint32_t> pixels =
        placeholderKind == IconPlaceholderKind::Folder
            ? BuildFolderPlaceholderPixels()
            : BuildFilePlaceholderPixels();
    placeholder = CreatePixelBitmap(
        target,
        pixels.data(),
        48,
        48);
    return placeholder.Get();
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> IconCache::CreatePixelBitmap(
    ID2D1RenderTarget* target,
    const std::uint32_t* pixels,
    UINT width,
    UINT height) {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> result;
    if (target == nullptr || pixels == nullptr || width == 0 || height == 0) {
        return result;
    }
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f);
    target->CreateBitmap(
        D2D1::SizeU(width, height),
        pixels,
        width * sizeof(std::uint32_t),
        &properties,
        result.GetAddressOf());
    return result;
}

void IconCache::ProcessCompleted(ID2D1RenderTarget* target) {
    if (target == nullptr) {
        return;
    }
    std::unordered_map<std::wstring, CompletedIcon> completed;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        completed.swap(asyncState_->completed);
        asyncState_->invalidatePosted = false;
    }
    for (auto& [path, completedIcon] : completed) {
        const std::uint64_t completedGeneration = completedIcon.generation;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap =
            LoadIconBitmap(target, completedIcon.icon);
        completedIcon.Reset();
        std::scoped_lock lock(cacheMutex_, asyncState_->mutex);
        if (!asyncState_->active ||
            completedGeneration != asyncState_->generation) {
            continue;
        }
        if (bitmap == nullptr) {
            RecordFailedIconLoadLocked(
                *asyncState_,
                path,
                completedGeneration);
            continue;
        }
        asyncState_->failedByPath.erase(path);
        const auto existing = cache_.find(path);
        if (existing != cache_.end()) {
            existing->second = std::move(bitmap);
            continue;
        }
        if (cache_.size() >= capacity_) {
            cache_.erase(cache_.begin());
        }
        cache_[path] = std::move(bitmap);
    }
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> IconCache::LoadIconBitmap(ID2D1RenderTarget* target, HICON icon) {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> result;
    if (wicFactory_ == nullptr || target == nullptr || icon == nullptr) {
        return result;
    }

    Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wicFactory_->CreateBitmapFromHICON(icon, wicBitmap.GetAddressOf()))) {
        return result;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(
            wicBitmap.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut))) {
        return result;
    }
    target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, result.GetAddressOf());
    return result;
}
