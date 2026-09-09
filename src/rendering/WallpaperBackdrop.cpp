#include "rendering/WallpaperBackdrop.h"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int kBlurRadius = 14;
constexpr int kBlurPasses = 3;

struct BgraPixel {
    BYTE blue = 0;
    BYTE green = 0;
    BYTE red = 0;
    BYTE alpha = 255;
};

struct DecodedWallpaper {
    UINT width = 0;
    UINT height = 0;
    std::vector<BYTE> pixels;
};

struct WallpaperDescription {
    std::wstring path;
    RECT monitorRect{};
    RECT layoutRect{};
    DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
    COLORREF backgroundColor = RGB(0, 0, 0);
};

long long IntersectionArea(const RECT& first, const RECT& second) {
    const long width = (std::max)(0L, (std::min)(first.right, second.right) - (std::max)(first.left, second.left));
    const long height = (std::max)(0L, (std::min)(first.bottom, second.bottom) - (std::max)(first.top, second.top));
    return static_cast<long long>(width) * static_cast<long long>(height);
}

RECT VirtualScreenRect() {
    const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return RECT{left, top, left + width, top + height};
}

bool IsRegularFile(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ResolveCachedWallpaper(const WallpaperDescription& description) {
    PWSTR rawRoamingPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &rawRoamingPath)) ||
        rawRoamingPath == nullptr) {
        return L"";
    }
    const std::wstring themesDirectory = std::wstring(rawRoamingPath) + L"\\Microsoft\\Windows\\Themes";
    CoTaskMemFree(rawRoamingPath);

    const LONG monitorWidth = description.monitorRect.right - description.monitorRect.left;
    const LONG monitorHeight = description.monitorRect.bottom - description.monitorRect.top;
    const std::wstring exactCache = themesDirectory + L"\\CachedFiles\\CachedImage_" +
        std::to_wstring(monitorWidth) + L"_" + std::to_wstring(monitorHeight) + L"_POS" +
        std::to_wstring(static_cast<int>(description.position)) + L".jpg";
    if (IsRegularFile(exactCache)) {
        return exactCache;
    }

    const std::wstring transcodedWallpaper = themesDirectory + L"\\TranscodedWallpaper";
    return IsRegularFile(transcodedWallpaper) ? transcodedWallpaper : L"";
}

bool ResolveWallpaper(HWND hwnd, WallpaperDescription& description) {
    const HMONITOR windowMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (windowMonitor == nullptr || !GetMonitorInfoW(windowMonitor, &monitorInfo)) {
        return false;
    }
    description.monitorRect = monitorInfo.rcMonitor;
    description.layoutRect = monitorInfo.rcMonitor;

    Microsoft::WRL::ComPtr<IDesktopWallpaper> desktopWallpaper;
    if (FAILED(CoCreateInstance(
            CLSID_DesktopWallpaper,
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(desktopWallpaper.GetAddressOf())))) {
        return false;
    }

    desktopWallpaper->GetPosition(&description.position);
    desktopWallpaper->GetBackgroundColor(&description.backgroundColor);

    std::wstring monitorId;
    UINT monitorCount = 0;
    if (SUCCEEDED(desktopWallpaper->GetMonitorDevicePathCount(&monitorCount))) {
        long long bestArea = -1;
        for (UINT index = 0; index < monitorCount; ++index) {
            LPWSTR rawMonitorId = nullptr;
            if (FAILED(desktopWallpaper->GetMonitorDevicePathAt(index, &rawMonitorId)) || rawMonitorId == nullptr) {
                continue;
            }
            RECT candidateRect{};
            if (SUCCEEDED(desktopWallpaper->GetMonitorRECT(rawMonitorId, &candidateRect))) {
                const long long area = IntersectionArea(candidateRect, monitorInfo.rcMonitor);
                if (area > bestArea) {
                    bestArea = area;
                    monitorId = rawMonitorId;
                }
            }
            CoTaskMemFree(rawMonitorId);
        }
    }

    LPWSTR rawWallpaperPath = nullptr;
    HRESULT wallpaperResult = E_FAIL;
    if (!monitorId.empty()) {
        wallpaperResult = desktopWallpaper->GetWallpaper(monitorId.c_str(), &rawWallpaperPath);
    }
    if (FAILED(wallpaperResult) || rawWallpaperPath == nullptr || rawWallpaperPath[0] == L'\0') {
        if (rawWallpaperPath != nullptr) {
            CoTaskMemFree(rawWallpaperPath);
            rawWallpaperPath = nullptr;
        }
        wallpaperResult = desktopWallpaper->GetWallpaper(nullptr, &rawWallpaperPath);
    }
    if (SUCCEEDED(wallpaperResult) && rawWallpaperPath != nullptr) {
        description.path = rawWallpaperPath;
    }
    if (rawWallpaperPath != nullptr) {
        CoTaskMemFree(rawWallpaperPath);
    }

    if (!IsRegularFile(description.path)) {
        description.path = ResolveCachedWallpaper(description);
    }
    if (!IsRegularFile(description.path)) {
        wchar_t fallbackPath[MAX_PATH]{};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, fallbackPath, 0) && IsRegularFile(fallbackPath)) {
            description.path = fallbackPath;
        }
    }
    if (description.position == DWPOS_SPAN) {
        description.layoutRect = VirtualScreenRect();
    }
    return IsRegularFile(description.path);
}

bool DecodeWallpaper(const std::wstring& path, DecodedWallpaper& image) {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf())))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            decoder.GetAddressOf()))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(frame->GetSize(&image.width, &image.height)) ||
        image.width == 0 || image.height == 0) {
        return false;
    }
    if (image.width > (std::numeric_limits<UINT>::max)() / 4U) {
        return false;
    }
    const UINT stride = image.width * 4U;
    if (image.height > (std::numeric_limits<UINT>::max)() / stride) {
        return false;
    }
    const UINT byteCount = stride * image.height;

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom))) {
        return false;
    }

    image.pixels.resize(byteCount);
    return SUCCEEDED(converter->CopyPixels(nullptr, stride, byteCount, image.pixels.data()));
}

bool FileWriteTime(const std::wstring& path, FILETIME& writeTime) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return false;
    }
    writeTime = attributes.ftLastWriteTime;
    return true;
}

constexpr UINT kDecodedWallpaperReleaseDelayMilliseconds = 750;

struct DecodedWallpaperCache {
    std::mutex mutex;
    std::wstring path;
    FILETIME writeTime{};
    std::shared_ptr<const DecodedWallpaper> image;
    UINT_PTR releaseTimer = 0;
};

DecodedWallpaperCache& SharedDecodedWallpaperCache() {
    static DecodedWallpaperCache cache;
    return cache;
}

void CALLBACK ReleaseDecodedWallpaperCache(
    HWND,
    UINT,
    UINT_PTR timerId,
    DWORD) {
    DecodedWallpaperCache& cache = SharedDecodedWallpaperCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.releaseTimer != timerId) {
        return;
    }
    KillTimer(nullptr, timerId);
    cache.releaseTimer = 0;
    cache.image.reset();
    cache.path.clear();
    cache.writeTime = {};
}

void ScheduleDecodedWallpaperReleaseLocked(
    DecodedWallpaperCache& cache) {
    if (cache.releaseTimer != 0) {
        KillTimer(nullptr, cache.releaseTimer);
        cache.releaseTimer = 0;
    }
    cache.releaseTimer = SetTimer(
        nullptr,
        0,
        kDecodedWallpaperReleaseDelayMilliseconds,
        ReleaseDecodedWallpaperCache);
    if (cache.releaseTimer == 0) {
        cache.image.reset();
        cache.path.clear();
        cache.writeTime = {};
    }
}

std::shared_ptr<const DecodedWallpaper> CachedWallpaper(const std::wstring& path) {
    DecodedWallpaperCache& cache = SharedDecodedWallpaperCache();
    FILETIME writeTime{};
    if (!FileWriteTime(path, writeTime)) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.image != nullptr &&
            CompareStringOrdinal(cache.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL &&
            CompareFileTime(&cache.writeTime, &writeTime) == 0) {
            const std::shared_ptr<const DecodedWallpaper> result = cache.image;
            ScheduleDecodedWallpaperReleaseLocked(cache);
            return result;
        }
    }

    auto decoded = std::make_shared<DecodedWallpaper>();
    if (!DecodeWallpaper(path, *decoded)) {
        return {};
    }
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.path = path;
    cache.writeTime = writeTime;
    cache.image = decoded;
    const std::shared_ptr<const DecodedWallpaper> result = cache.image;
    ScheduleDecodedWallpaperReleaseLocked(cache);
    return result;
}

BgraPixel BackgroundPixel(COLORREF color) {
    return BgraPixel{
        GetBValue(color),
        GetGValue(color),
        GetRValue(color),
        255,
    };
}

BgraPixel PixelAt(const DecodedWallpaper& image, UINT x, UINT y) {
    const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4U;
    return BgraPixel{
        image.pixels[offset],
        image.pixels[offset + 1U],
        image.pixels[offset + 2U],
        255,
    };
}

BYTE InterpolateChannel(BYTE topLeft, BYTE topRight, BYTE bottomLeft, BYTE bottomRight, double x, double y) {
    const double top = static_cast<double>(topLeft) +
        (static_cast<double>(topRight) - static_cast<double>(topLeft)) * x;
    const double bottom = static_cast<double>(bottomLeft) +
        (static_cast<double>(bottomRight) - static_cast<double>(bottomLeft)) * x;
    const double value = top + (bottom - top) * y;
    return static_cast<BYTE>(std::clamp(std::lround(value), 0L, 255L));
}

BgraPixel SampleBilinear(const DecodedWallpaper& image, double sourceX, double sourceY) {
    sourceX = std::clamp(sourceX, 0.0, static_cast<double>(image.width - 1U));
    sourceY = std::clamp(sourceY, 0.0, static_cast<double>(image.height - 1U));
    const UINT left = static_cast<UINT>(std::floor(sourceX));
    const UINT top = static_cast<UINT>(std::floor(sourceY));
    const UINT right = (std::min)(left + 1U, image.width - 1U);
    const UINT bottom = (std::min)(top + 1U, image.height - 1U);
    const double fractionX = sourceX - static_cast<double>(left);
    const double fractionY = sourceY - static_cast<double>(top);
    const BgraPixel topLeft = PixelAt(image, left, top);
    const BgraPixel topRight = PixelAt(image, right, top);
    const BgraPixel bottomLeft = PixelAt(image, left, bottom);
    const BgraPixel bottomRight = PixelAt(image, right, bottom);
    return BgraPixel{
        InterpolateChannel(topLeft.blue, topRight.blue, bottomLeft.blue, bottomRight.blue, fractionX, fractionY),
        InterpolateChannel(topLeft.green, topRight.green, bottomLeft.green, bottomRight.green, fractionX, fractionY),
        InterpolateChannel(topLeft.red, topRight.red, bottomLeft.red, bottomRight.red, fractionX, fractionY),
        255,
    };
}

bool ScaledSourceCoordinates(
    const DecodedWallpaper& image,
    const RECT& layoutRect,
    double screenX,
    double screenY,
    bool fill,
    double& sourceX,
    double& sourceY) {
    const double layoutWidth = static_cast<double>(layoutRect.right - layoutRect.left);
    const double layoutHeight = static_cast<double>(layoutRect.bottom - layoutRect.top);
    if (layoutWidth <= 0.0 || layoutHeight <= 0.0) {
        return false;
    }
    const double scaleX = layoutWidth / static_cast<double>(image.width);
    const double scaleY = layoutHeight / static_cast<double>(image.height);
    const double scale = fill ? (std::max)(scaleX, scaleY) : (std::min)(scaleX, scaleY);
    const double displayedWidth = static_cast<double>(image.width) * scale;
    const double displayedHeight = static_cast<double>(image.height) * scale;
    const double originX = static_cast<double>(layoutRect.left) + (layoutWidth - displayedWidth) * 0.5;
    const double originY = static_cast<double>(layoutRect.top) + (layoutHeight - displayedHeight) * 0.5;
    if (!fill &&
        (screenX < originX || screenY < originY || screenX >= originX + displayedWidth || screenY >= originY + displayedHeight)) {
        return false;
    }
    sourceX = (screenX - originX) / scale - 0.5;
    sourceY = (screenY - originY) / scale - 0.5;
    return true;
}

BgraPixel SampleWallpaper(
    const DecodedWallpaper& image,
    const WallpaperDescription& description,
    double screenX,
    double screenY) {
    const RECT& layout = description.layoutRect;
    const double layoutWidth = static_cast<double>(layout.right - layout.left);
    const double layoutHeight = static_cast<double>(layout.bottom - layout.top);
    double sourceX = 0.0;
    double sourceY = 0.0;
    bool inside = true;

    switch (description.position) {
        case DWPOS_TILE: {
            double localX = std::fmod(screenX - static_cast<double>(layout.left), static_cast<double>(image.width));
            double localY = std::fmod(screenY - static_cast<double>(layout.top), static_cast<double>(image.height));
            if (localX < 0.0) {
                localX += image.width;
            }
            if (localY < 0.0) {
                localY += image.height;
            }
            sourceX = localX - 0.5;
            sourceY = localY - 0.5;
            break;
        }
        case DWPOS_STRETCH:
            if (layoutWidth <= 0.0 || layoutHeight <= 0.0) {
                inside = false;
                break;
            }
            sourceX = (screenX - static_cast<double>(layout.left)) * image.width / layoutWidth - 0.5;
            sourceY = (screenY - static_cast<double>(layout.top)) * image.height / layoutHeight - 0.5;
            break;
        case DWPOS_FIT:
            inside = ScaledSourceCoordinates(image, layout, screenX, screenY, false, sourceX, sourceY);
            break;
        case DWPOS_FILL:
        case DWPOS_SPAN:
            inside = ScaledSourceCoordinates(image, layout, screenX, screenY, true, sourceX, sourceY);
            break;
        case DWPOS_CENTER:
        default: {
            const double originX = static_cast<double>(layout.left) +
                (layoutWidth - static_cast<double>(image.width)) * 0.5;
            const double originY = static_cast<double>(layout.top) +
                (layoutHeight - static_cast<double>(image.height)) * 0.5;
            sourceX = screenX - originX - 0.5;
            sourceY = screenY - originY - 0.5;
            inside = sourceX >= -0.5 && sourceY >= -0.5 &&
                sourceX < static_cast<double>(image.width) - 0.5 &&
                sourceY < static_cast<double>(image.height) - 0.5;
            break;
        }
    }
    return inside ? SampleBilinear(image, sourceX, sourceY) : BackgroundPixel(description.backgroundColor);
}

void HorizontalBoxBlur(
    const std::vector<BYTE>& source,
    std::vector<BYTE>& destination,
    int width,
    int height,
    int radius) {
    const int diameter = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        int sums[3]{};
        for (int sample = -radius; sample <= radius; ++sample) {
            const int sourceX = std::clamp(sample, 0, width - 1);
            const size_t offset = (static_cast<size_t>(y) * width + sourceX) * 4U;
            for (int channel = 0; channel < 3; ++channel) {
                sums[channel] += source[offset + static_cast<size_t>(channel)];
            }
        }
        for (int x = 0; x < width; ++x) {
            const size_t outputOffset = (static_cast<size_t>(y) * width + x) * 4U;
            for (int channel = 0; channel < 3; ++channel) {
                destination[outputOffset + static_cast<size_t>(channel)] = static_cast<BYTE>(sums[channel] / diameter);
            }
            destination[outputOffset + 3U] = 255;

            const int removeX = std::clamp(x - radius, 0, width - 1);
            const int addX = std::clamp(x + radius + 1, 0, width - 1);
            const size_t removeOffset = (static_cast<size_t>(y) * width + removeX) * 4U;
            const size_t addOffset = (static_cast<size_t>(y) * width + addX) * 4U;
            for (int channel = 0; channel < 3; ++channel) {
                sums[channel] += source[addOffset + static_cast<size_t>(channel)] -
                    source[removeOffset + static_cast<size_t>(channel)];
            }
        }
    }
}

void VerticalBoxBlur(
    const std::vector<BYTE>& source,
    std::vector<BYTE>& destination,
    int width,
    int height,
    int radius) {
    const int diameter = radius * 2 + 1;
    for (int x = 0; x < width; ++x) {
        int sums[3]{};
        for (int sample = -radius; sample <= radius; ++sample) {
            const int sourceY = std::clamp(sample, 0, height - 1);
            const size_t offset = (static_cast<size_t>(sourceY) * width + x) * 4U;
            for (int channel = 0; channel < 3; ++channel) {
                sums[channel] += source[offset + static_cast<size_t>(channel)];
            }
        }
        for (int y = 0; y < height; ++y) {
            const size_t outputOffset = (static_cast<size_t>(y) * width + x) * 4U;
            for (int channel = 0; channel < 3; ++channel) {
                destination[outputOffset + static_cast<size_t>(channel)] = static_cast<BYTE>(sums[channel] / diameter);
            }
            destination[outputOffset + 3U] = 255;

            const int removeY = std::clamp(y - radius, 0, height - 1);
            const int addY = std::clamp(y + radius + 1, 0, height - 1);
            const size_t removeOffset = (static_cast<size_t>(removeY) * width + x) * 4U;
            const size_t addOffset = (static_cast<size_t>(addY) * width + x) * 4U;
            for (int channel = 0; channel < 3; ++channel) {
                sums[channel] += source[addOffset + static_cast<size_t>(channel)] -
                    source[removeOffset + static_cast<size_t>(channel)];
            }
        }
    }
}

void BlurPixels(std::vector<BYTE>& pixels, int width, int height) {
    std::vector<BYTE> temporary(pixels.size());
    for (int pass = 0; pass < kBlurPasses; ++pass) {
        HorizontalBoxBlur(pixels, temporary, width, height, kBlurRadius);
        VerticalBoxBlur(temporary, pixels, width, height, kBlurRadius);
    }
}

}  // namespace

bool WallpaperBackdrop::Refresh(
    HWND hwnd,
    ID2D1RenderTarget* renderTarget,
    int minimumWidthPixels,
    int minimumHeightPixels,
    bool blur) {
    bitmap_.Reset();
    pixelSize_ = {};
    if (hwnd == nullptr || renderTarget == nullptr) {
        return false;
    }

    WallpaperDescription description;
    if (!ResolveWallpaper(hwnd, description)) {
        return false;
    }
    const std::shared_ptr<const DecodedWallpaper> wallpaper =
        CachedWallpaper(description.path);
    if (wallpaper == nullptr) {
        return false;
    }

    RECT clientRect{};
    POINT clientOrigin{};
    if (!GetClientRect(hwnd, &clientRect) || !ClientToScreen(hwnd, &clientOrigin)) {
        return false;
    }
    const int outputWidth = (std::max)(
        static_cast<int>(clientRect.right - clientRect.left),
        minimumWidthPixels);
    const int outputHeight = (std::max)(
        static_cast<int>(clientRect.bottom - clientRect.top),
        minimumHeightPixels);
    if (outputWidth <= 0 || outputHeight <= 0) {
        return false;
    }

    const int padding = blur ? kBlurRadius * kBlurPasses : 0;
    const int sampleWidth = outputWidth + padding * 2;
    const int sampleHeight = outputHeight + padding * 2;
    if (sampleWidth <= 0 || sampleHeight <= 0 ||
        static_cast<size_t>(sampleWidth) > (std::numeric_limits<size_t>::max)() /
            static_cast<size_t>(sampleHeight) / 4U) {
        return false;
    }

    std::vector<BYTE> sampled(static_cast<size_t>(sampleWidth) * sampleHeight * 4U);
    for (int y = 0; y < sampleHeight; ++y) {
        const double screenY = static_cast<double>(clientOrigin.y - padding + y) + 0.5;
        for (int x = 0; x < sampleWidth; ++x) {
            const double screenX = static_cast<double>(clientOrigin.x - padding + x) + 0.5;
            const BgraPixel pixel = SampleWallpaper(*wallpaper, description, screenX, screenY);
            const size_t offset = (static_cast<size_t>(y) * sampleWidth + x) * 4U;
            sampled[offset] = pixel.blue;
            sampled[offset + 1U] = pixel.green;
            sampled[offset + 2U] = pixel.red;
            sampled[offset + 3U] = 255;
        }
    }
    if (blur) {
        BlurPixels(sampled, sampleWidth, sampleHeight);
    }

    std::vector<BYTE> output(static_cast<size_t>(outputWidth) * outputHeight * 4U);
    const size_t outputStride = static_cast<size_t>(outputWidth) * 4U;
    const size_t sampleStride = static_cast<size_t>(sampleWidth) * 4U;
    for (int y = 0; y < outputHeight; ++y) {
        const BYTE* sourceRow = sampled.data() + static_cast<size_t>(y + padding) * sampleStride +
            static_cast<size_t>(padding) * 4U;
        BYTE* destinationRow = output.data() + static_cast<size_t>(y) * outputStride;
        std::copy_n(sourceRow, outputStride, destinationRow);
    }

    const FLOAT dpi = static_cast<FLOAT>((std::max)(GetDpiForWindow(hwnd), 96U));
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi,
        dpi);
    const bool created = SUCCEEDED(renderTarget->CreateBitmap(
        D2D1::SizeU(static_cast<UINT>(outputWidth), static_cast<UINT>(outputHeight)),
        output.data(),
        static_cast<UINT32>(outputStride),
        properties,
        bitmap_.GetAddressOf()));
    if (created) {
        pixelSize_ = SIZE{outputWidth, outputHeight};
    }
    return created;
}

bool CalculateWallpaperBackdropDrawGeometry(
    D2D1_SIZE_F bitmapSize,
    const D2D1_RECT_F& destination,
    WallpaperBackdropDrawMode mode,
    WallpaperBackdropDrawGeometry& geometry) noexcept {
    const FLOAT destinationWidth = destination.right - destination.left;
    const FLOAT destinationHeight = destination.bottom - destination.top;
    if (bitmapSize.width <= 0.0f || bitmapSize.height <= 0.0f ||
        destinationWidth <= 0.0f || destinationHeight <= 0.0f) {
        return false;
    }
    if (mode == WallpaperBackdropDrawMode::StretchToDestination) {
        geometry.destination = destination;
        geometry.source = D2D1::RectF(
            0.0f, 0.0f, bitmapSize.width, bitmapSize.height);
        return true;
    }

    const FLOAT visibleWidth =
        (std::min)(bitmapSize.width, destinationWidth);
    const FLOAT visibleHeight =
        (std::min)(bitmapSize.height, destinationHeight);
    geometry.destination = D2D1::RectF(
        destination.left,
        destination.top,
        destination.left + visibleWidth,
        destination.top + visibleHeight);
    geometry.source = D2D1::RectF(
        0.0f, 0.0f, visibleWidth, visibleHeight);
    return true;
}

bool WallpaperBackdrop::Draw(
    ID2D1RenderTarget* renderTarget,
    const D2D1_RECT_F& destination,
    WallpaperBackdropDrawMode mode) const {
    if (renderTarget == nullptr || bitmap_ == nullptr) {
        return false;
    }
    WallpaperBackdropDrawGeometry geometry{};
    if (!CalculateWallpaperBackdropDrawGeometry(
            bitmap_->GetSize(), destination, mode, geometry)) {
        return false;
    }
    renderTarget->DrawBitmap(
        bitmap_.Get(),
        geometry.destination,
        1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        &geometry.source);
    return true;
}

bool WallpaperBackdrop::CoversPixels(int width, int height) const noexcept {
    return bitmap_ != nullptr && pixelSize_.cx >= width && pixelSize_.cy >= height;
}
