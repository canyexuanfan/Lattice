#include "desktop/DesktopLayout.h"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <CommCtrl.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

std::wstring FullPath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return path;
    }
    std::wstring result(required, L'\0');
    const DWORD copied = GetFullPathNameW(path.c_str(), required, result.data(), nullptr);
    if (copied == 0 || copied >= required) {
        return path;
    }
    result.resize(copied);
    return result;
}

bool PathsEqual(const std::wstring& left, const std::wstring& right) {
    const std::wstring a = FullPath(left);
    const std::wstring b = FullPath(right);
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring ParentDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

std::wstring FileName(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

void RefreshDesktopDirectoriesAsynchronously(const std::vector<DesktopPosition>& positions) {
    std::vector<std::wstring> directories;
    directories.reserve(positions.size());
    for (const DesktopPosition& position : positions) {
        const std::wstring directory = ParentDirectory(position.path);
        if (directory.empty()) {
            continue;
        }
        const bool alreadyQueued = std::any_of(
            directories.begin(),
            directories.end(),
            [&](const std::wstring& existing) { return PathsEqual(existing, directory); });
        if (!alreadyQueued) {
            directories.push_back(directory);
        }
    }
    for (const std::wstring& directory : directories) {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, directory.c_str(), nullptr);
    }
}

void RefreshFolderView(IFolderView* view) {
    if (view == nullptr) {
        return;
    }
    ComPtr<IShellView> shellView;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&shellView))) && shellView != nullptr) {
        shellView->Refresh();
    }
}

class ExactDesktopPositionMode {
public:
    bool Begin(IFolderView* view, std::wstring& errorMessage) {
        view_.Reset();
        if (view == nullptr ||
            FAILED(view->QueryInterface(IID_PPV_ARGS(&view_))) ||
            view_ == nullptr) {
            errorMessage = L"Explorer 桌面视图不支持精确图标定位。";
            return false;
        }
        DWORD flags = 0;
        if (FAILED(view_->GetCurrentFolderFlags(&flags))) {
            errorMessage = L"无法读取 Explorer 桌面对齐设置。";
            view_.Reset();
            return false;
        }
        if ((flags & FWF_SNAPTOGRID) == 0) {
            return true;
        }
        if (FAILED(view_->SetCurrentFolderFlags(FWF_SNAPTOGRID, 0))) {
            errorMessage = L"无法关闭当前 Explorer 桌面视图的坐标吸附。";
            view_.Reset();
            return false;
        }
        return true;
    }

private:
    ComPtr<IFolderView2> view_;
};

HRESULT GetDesktopFolderView(
    ComPtr<IFolderView>& folderView,
    HWND* viewWindow = nullptr) {
    ComPtr<IShellWindows> shellWindows;
    HRESULT result = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows));
    if (FAILED(result)) {
        return result;
    }

    VARIANT location{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT root{};
    root.vt = VT_EMPTY;
    long desktopHwnd = 0;
    ComPtr<IDispatch> dispatch;
    result = shellWindows->FindWindowSW(&location, &root, SWC_DESKTOP, &desktopHwnd, SWFO_NEEDDISPATCH, &dispatch);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IServiceProvider> serviceProvider;
    result = dispatch.As(&serviceProvider);
    if (FAILED(result)) {
        return result;
    }
    ComPtr<IShellBrowser> shellBrowser;
    result = serviceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<IShellView> shellView;
    result = shellBrowser->QueryActiveShellView(&shellView);
    if (FAILED(result)) {
        return result;
    }
    if (viewWindow != nullptr) {
        *viewWindow = nullptr;
        result = shellView->GetWindow(viewWindow);
        if (FAILED(result)) {
            return result;
        }
    }
    return shellView.As(&folderView);
}

bool ItemPath(IShellFolder* folder, PCUITEMID_CHILD item, std::wstring& path) {
    STRRET displayName{};
    if (FAILED(folder->GetDisplayNameOf(item, SHGDN_FORPARSING, &displayName))) {
        return false;
    }
    std::vector<wchar_t> buffer(32768);
    if (FAILED(StrRetToBufW(&displayName, item, buffer.data(), static_cast<UINT>(buffer.size())))) {
        return false;
    }
    path = buffer.data();
    return !path.empty();
}

struct ShellDesktopItem {
    PIDLIST_RELATIVE pidl = nullptr;
    std::wstring path;
    int viewIndex = -1;

    ShellDesktopItem() = default;
    ShellDesktopItem(const ShellDesktopItem&) = delete;
    ShellDesktopItem& operator=(const ShellDesktopItem&) = delete;
    ShellDesktopItem(ShellDesktopItem&& other) noexcept
        : pidl(other.pidl),
          path(std::move(other.path)),
          viewIndex(other.viewIndex) {
        other.pidl = nullptr;
        other.viewIndex = -1;
    }
    ~ShellDesktopItem() { CoTaskMemFree(pidl); }
};

bool ParseDesktopViewItem(
    IFolderView* view,
    const std::wstring& path,
    ShellDesktopItem& item) {
    if (view == nullptr) {
        return false;
    }
    ComPtr<IShellFolder> folder;
    if (FAILED(view->GetFolder(IID_PPV_ARGS(&folder))) || folder == nullptr) {
        return false;
    }
    std::wstring leafName = FileName(path);
    if (leafName.empty()) {
        return false;
    }
    PIDLIST_RELATIVE parsed = nullptr;
    if (FAILED(folder->ParseDisplayName(
            nullptr,
            nullptr,
            leafName.data(),
            nullptr,
            &parsed,
            nullptr)) ||
        parsed == nullptr) {
        return false;
    }
    if (ILIsEmpty(parsed) || !ILIsEmpty(ILNext(parsed))) {
        CoTaskMemFree(parsed);
        return false;
    }
    std::wstring parsedPath;
    if (!ItemPath(folder.Get(), parsed, parsedPath) ||
        !PathsEqual(parsedPath, path)) {
        CoTaskMemFree(parsed);
        return false;
    }
    item.pidl = parsed;
    item.path = std::move(parsedPath);
    return true;
}

bool EnumerateDesktopItems(IFolderView* view, std::vector<ShellDesktopItem>& items, std::wstring& errorMessage) {
    ComPtr<IShellFolder> folder;
    HRESULT result = view->GetFolder(IID_PPV_ARGS(&folder));
    if (FAILED(result)) {
        errorMessage = L"无法访问 Explorer 桌面目录。";
        return false;
    }
    int count = 0;
    result = view->ItemCount(SVGIO_ALLVIEW, &count);
    if (FAILED(result)) {
        errorMessage = L"无法读取 Explorer 桌面项目数量。";
        return false;
    }
    items.clear();
    items.reserve(static_cast<size_t>(std::max(count, 0)));
    for (int index = 0; index < count; ++index) {
        ShellDesktopItem item;
        item.viewIndex = index;
        if (SUCCEEDED(view->Item(index, &item.pidl)) && item.pidl != nullptr && ItemPath(folder.Get(), item.pidl, item.path)) {
            items.push_back(std::move(item));
        }
    }
    return true;
}

}  // namespace

bool DesktopLayout::CaptureAllPositions(
    std::vector<DesktopPosition>& positions,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    positions.clear();
    ComPtr<IFolderView> view;
    if (FAILED(GetDesktopFolderView(view))) {
        errorMessage = L"无法连接 Explorer 桌面视图，无法建立桌面布局快照。";
        return false;
    }
    std::vector<ShellDesktopItem> items;
    if (!EnumerateDesktopItems(view.Get(), items, errorMessage)) {
        return false;
    }
    positions.reserve(items.size());
    for (const ShellDesktopItem& item : items) {
        POINT point{};
        if (SUCCEEDED(view->GetItemPosition(item.pidl, &point))) {
            positions.push_back(DesktopPosition{item.path, point});
        }
    }
    if (!items.empty() && positions.empty()) {
        errorMessage = L"Explorer 没有返回任何桌面项目坐标，无法建立桌面布局快照。";
        return false;
    }
    return true;
}

bool DesktopLayout::CapturePosition(const std::wstring& path, POINT& point, std::wstring& errorMessage) const {
    errorMessage.clear();
    ComPtr<IFolderView> view;
    if (FAILED(GetDesktopFolderView(view))) {
        errorMessage = L"无法连接 Explorer 桌面视图，未移动该项目。";
        return false;
    }
    ShellDesktopItem parsedItem;
    if (ParseDesktopViewItem(view.Get(), path, parsedItem)) {
        POINT parsedPoint{};
        if (SUCCEEDED(view->GetItemPosition(parsedItem.pidl, &parsedPoint))) {
            point = parsedPoint;
            return true;
        }
    }
    std::vector<ShellDesktopItem> items;
    if (!EnumerateDesktopItems(view.Get(), items, errorMessage)) {
        return false;
    }
    for (const ShellDesktopItem& item : items) {
        if (PathsEqual(item.path, path)) {
            if (SUCCEEDED(view->GetItemPosition(item.pidl, &point))) {
                return true;
            }
            errorMessage = L"Explorer 没有返回该桌面项目的坐标，未移动该项目。";
            return false;
        }
    }
    errorMessage = L"Explorer 桌面视图中没有找到该项目，未移动该项目。";
    return false;
}

bool DesktopLayout::CaptureScreenPosition(
    const std::wstring& path,
    POINT& point,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    ComPtr<IFolderView> view;
    HWND viewHwnd = nullptr;
    if (FAILED(GetDesktopFolderView(view, &viewHwnd))) {
        errorMessage = L"无法连接 Explorer 桌面视图，未读取该项目的屏幕坐标。";
        return false;
    }

    POINT viewPoint{};
    ShellDesktopItem parsedItem;
    bool captured = false;
    if (ParseDesktopViewItem(view.Get(), path, parsedItem) &&
        SUCCEEDED(view->GetItemPosition(parsedItem.pidl, &viewPoint))) {
        captured = true;
    } else {
        std::vector<ShellDesktopItem> items;
        if (!EnumerateDesktopItems(view.Get(), items, errorMessage)) {
            return false;
        }
        for (const ShellDesktopItem& item : items) {
            if (!PathsEqual(item.path, path)) {
                continue;
            }
            if (SUCCEEDED(view->GetItemPosition(item.pidl, &viewPoint))) {
                captured = true;
            }
            break;
        }
    }
    if (!captured) {
        errorMessage = L"Explorer 桌面视图中没有找到该项目的屏幕坐标。";
        return false;
    }

    const HWND desktopListView =
        viewHwnd == nullptr
            ? nullptr
            : FindWindowExW(
                  viewHwnd,
                  nullptr,
                  L"SysListView32",
                  nullptr);
    if (desktopListView == nullptr ||
        IsWindow(desktopListView) == FALSE) {
        errorMessage = L"无法取得 Explorer 桌面视图窗口，未转换屏幕坐标。";
        return false;
    }
    point = viewPoint;
    if (ClientToScreen(desktopListView, &point) == FALSE) {
        errorMessage = L"无法把 Explorer 桌面视图坐标转换为屏幕坐标。";
        return false;
    }
    return true;
}

bool DesktopLayout::RestoreScreenPosition(
    const std::wstring& path,
    POINT screenPoint,
    POINT& restoredPoint,
    std::wstring& errorMessage,
    const std::function<void()>& onVisiblePositioned,
    const std::function<bool()>& cancellationRequested) const {
    errorMessage.clear();
    ExactDesktopPositionMode exactPositionMode;
    const auto isCancelled = [&]() {
        return cancellationRequested && cancellationRequested();
    };
    ComPtr<IFolderView> view;
    bool parsedPositionIssued = false;
    bool visiblePositionIssued = false;
    HWND desktopListView = nullptr;
    POINT exactViewPoint{};
    bool exactViewPointKnown = false;
    POINT previousConfirmed{};
    int stablePositionSamples = 0;
    ULONGLONG stableSince = 0;
    int independentAgreementSamples = 0;
    POINT independentCandidate{};
    bool independentAgreementReady = false;
    ULONGLONG nextIndependentConfirmationAt = 0;
    const ULONGLONG positioningStartedAt = GetTickCount64();
    ULONGLONG stabilizationStartedAt = 0;
    constexpr ULONGLONG kAppearanceTimeoutMilliseconds = 4000;
    constexpr ULONGLONG kStabilizationTimeoutMilliseconds = 2500;
    int pollAttempts = 0;
    bool visiblePositionReported = false;
    for (;;) {
        if (isCancelled()) {
            errorMessage = L"桌面定位已取消。";
            return false;
        }
        if (view == nullptr) {
            HWND shellViewWindow = nullptr;
            if (FAILED(GetDesktopFolderView(view, &shellViewWindow))) {
                view.Reset();
            } else {
                if (!exactPositionMode.Begin(view.Get(), errorMessage)) {
                    return false;
                }
                desktopListView = FindWindowExW(
                    shellViewWindow,
                    nullptr,
                    L"SysListView32",
                    nullptr);
                exactViewPoint = screenPoint;
                exactViewPointKnown =
                    desktopListView != nullptr &&
                    IsWindow(desktopListView) != FALSE &&
                    ScreenToClient(desktopListView, &exactViewPoint) != FALSE;
            }
        }
        if (view != nullptr) {
            constexpr DWORD kPositionFlags =
                SVSI_POSITIONITEM | SVSI_TRANSLATEPT | SVSI_NOSTATECHANGE;
            if (!parsedPositionIssued) {
                ShellDesktopItem parsedItem;
                if (ParseDesktopViewItem(view.Get(), path, parsedItem)) {
                    if (isCancelled()) {
                        errorMessage = L"桌面定位已取消。";
                        return false;
                    }
                    PCUITEMID_CHILD pidl = parsedItem.pidl;
                    POINT targetPoint = screenPoint;
                    if (SUCCEEDED(view->SelectAndPositionItems(
                            1, &pidl, &targetPoint, kPositionFlags))) {
                        parsedPositionIssued = true;
                    }
                }
            }

            std::vector<ShellDesktopItem> visibleItems;
            std::wstring enumerateError;
            if (!EnumerateDesktopItems(
                    view.Get(), visibleItems, enumerateError)) {
                view.Reset();
                desktopListView = nullptr;
                exactViewPointKnown = false;
                parsedPositionIssued = false;
                visiblePositionIssued = false;
                stablePositionSamples = 0;
                stableSince = 0;
                independentAgreementSamples = 0;
                independentAgreementReady = false;
                nextIndependentConfirmationAt = 0;
            } else {
                const auto visibleItem = std::find_if(
                    visibleItems.begin(),
                    visibleItems.end(),
                    [&](const ShellDesktopItem& candidate) {
                        return PathsEqual(candidate.path, path);
                    });
                if (visibleItem == visibleItems.end()) {
                    stablePositionSamples = 0;
                    stableSince = 0;
                    independentAgreementSamples = 0;
                    independentAgreementReady = false;
                    nextIndependentConfirmationAt = 0;
                } else {
                    bool positionedThisAttempt = false;
                    if (!visiblePositionIssued) {
                        if (isCancelled()) {
                            errorMessage = L"桌面定位已取消。";
                            return false;
                        }
                        DWORD_PTR directPositionResult = FALSE;
                        const bool directPositioned =
                            exactViewPointKnown &&
                            visibleItem->viewIndex >= 0 &&
                            exactViewPoint.x >= -32768 &&
                            exactViewPoint.x <= 32767 &&
                            exactViewPoint.y >= -32768 &&
                            exactViewPoint.y <= 32767 &&
                            SendMessageTimeoutW(
                                desktopListView,
                                LVM_SETITEMPOSITION,
                                static_cast<WPARAM>(visibleItem->viewIndex),
                                MAKELPARAM(
                                    static_cast<SHORT>(exactViewPoint.x),
                                    static_cast<SHORT>(exactViewPoint.y)),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                250,
                                &directPositionResult) != 0 &&
                            directPositionResult != FALSE;
                        PCUITEMID_CHILD pidl = visibleItem->pidl;
                        POINT targetPoint = screenPoint;
                        if (directPositioned ||
                            SUCCEEDED(view->SelectAndPositionItems(
                                1, &pidl, &targetPoint, kPositionFlags))) {
                            visiblePositionIssued = true;
                            if (stabilizationStartedAt == 0) {
                                stabilizationStartedAt = GetTickCount64();
                            }
                            stablePositionSamples = 0;
                            stableSince = 0;
                            independentAgreementSamples = 0;
                            independentAgreementReady = false;
                            nextIndependentConfirmationAt = 0;
                            positionedThisAttempt = true;
                        }
                    }
                    POINT confirmed{};
                    if (visiblePositionIssued &&
                        !positionedThisAttempt &&
                        SUCCEEDED(view->GetItemPosition(
                            visibleItem->pidl, &confirmed))) {
                        if (!exactViewPointKnown ||
                            confirmed.x != exactViewPoint.x ||
                            confirmed.y != exactViewPoint.y) {
                            visiblePositionIssued = false;
                            stablePositionSamples = 0;
                            stableSince = 0;
                            independentAgreementSamples = 0;
                            independentAgreementReady = false;
                            nextIndependentConfirmationAt = 0;
                            const ULONGLONG mismatchNow = GetTickCount64();
                            if (stabilizationStartedAt != 0 &&
                                mismatchNow - stabilizationStartedAt >=
                                    kStabilizationTimeoutMilliseconds) {
                                break;
                            }
                            Sleep(8);
                            ++pollAttempts;
                            continue;
                        }
                        const ULONGLONG now = GetTickCount64();
                        if (stablePositionSamples > 0 &&
                            confirmed.x == previousConfirmed.x &&
                            confirmed.y == previousConfirmed.y) {
                            ++stablePositionSamples;
                        } else {
                            previousConfirmed = confirmed;
                            stablePositionSamples = 1;
                            stableSince = now;
                            independentAgreementSamples = 0;
                            independentAgreementReady = false;
                            nextIndependentConfirmationAt = 0;
                        }
                        if (stablePositionSamples >= 3 &&
                            now - stableSince >= 160 &&
                            independentAgreementReady &&
                            confirmed.x == independentCandidate.x &&
                            confirmed.y == independentCandidate.y) {
                            for (int restoredSample = 0;
                                 restoredSample < 3;
                                 ++restoredSample) {
                                if (restoredSample != 0) {
                                    Sleep(40);
                                }
                                if (isCancelled()) {
                                    errorMessage = L"桌面定位已取消。";
                                    return false;
                                }
                                POINT afterRestore{};
                                if (FAILED(view->GetItemPosition(
                                        visibleItem->pidl,
                                        &afterRestore)) ||
                                    !exactViewPointKnown ||
                                    afterRestore.x != exactViewPoint.x ||
                                    afterRestore.y != exactViewPoint.y) {
                                    errorMessage =
                                        L"Explorer 精确定位后改变了图标位置。";
                                    return false;
                                }
                                confirmed = afterRestore;
                            }
                            restoredPoint = confirmed;
                            if (!visiblePositionReported) {
                                visiblePositionReported = true;
                                if (onVisiblePositioned) {
                                    onVisiblePositioned();
                                }
                            }
                            return true;
                        }
                        if (stablePositionSamples >= 3 &&
                            now - stableSince >= 160 &&
                            !independentAgreementReady &&
                            now >= nextIndependentConfirmationAt) {
                            if (isCancelled()) {
                                errorMessage = L"桌面定位已取消。";
                                return false;
                            }
                            POINT independentlyConfirmed{};
                            std::wstring independentError;
                            if (CapturePosition(
                                    path,
                                    independentlyConfirmed,
                                    independentError) &&
                                independentlyConfirmed.x == confirmed.x &&
                                independentlyConfirmed.y == confirmed.y) {
                                independentCandidate = independentlyConfirmed;
                                ++independentAgreementSamples;
                                independentAgreementReady =
                                    independentAgreementSamples >= 2;
                            } else {
                                independentAgreementSamples = 0;
                                independentAgreementReady = false;
                            }
                            nextIndependentConfirmationAt =
                                GetTickCount64() + 32;
                        }
                    } else {
                        stablePositionSamples = 0;
                        stableSince = 0;
                        independentAgreementSamples = 0;
                        independentAgreementReady = false;
                        nextIndependentConfirmationAt = 0;
                    }
                }
            }
        }
        if (isCancelled()) {
            errorMessage = L"桌面定位已取消。";
            return false;
        }
        const ULONGLONG now = GetTickCount64();
        const bool stabilizing = stabilizationStartedAt != 0;
        const ULONGLONG phaseStartedAt =
            stabilizing ? stabilizationStartedAt : positioningStartedAt;
        const ULONGLONG phaseTimeout = stabilizing
            ? kStabilizationTimeoutMilliseconds
            : kAppearanceTimeoutMilliseconds;
        const ULONGLONG phaseElapsed = now - phaseStartedAt;
        if (phaseElapsed >= phaseTimeout) {
            break;
        }
        const DWORD baseWaitMilliseconds = stabilizing
            ? 8
            : (phaseElapsed < 120 ? 8 : (pollAttempts < 10 ? 40 : 80));
        const DWORD waitMilliseconds = static_cast<DWORD>(
            (std::min)(
                static_cast<ULONGLONG>(baseWaitMilliseconds),
                phaseTimeout - phaseElapsed));
        Sleep((std::max<DWORD>)(1, waitMilliseconds));
        ++pollAttempts;
        if (isCancelled()) {
            errorMessage = L"桌面定位已取消。";
            return false;
        }
    }
    errorMessage = L"文件已安全归还桌面，但 Explorer 未能把图标放到鼠标释放位置。";
    return false;
}

bool DesktopLayout::RestorePositions(const std::vector<DesktopPosition>& positions, std::wstring& errorMessage) const {
    errorMessage.clear();
    if (positions.empty()) {
        return true;
    }
    RefreshDesktopDirectoriesAsynchronously(positions);
    for (int attempt = 0; attempt < 60; ++attempt) {
        ComPtr<IFolderView> view;
        if (SUCCEEDED(GetDesktopFolderView(view))) {
            std::vector<ShellDesktopItem> items;
            std::wstring enumerateError;
            if (EnumerateDesktopItems(view.Get(), items, enumerateError)) {
                size_t restored = 0;
                for (const DesktopPosition& position : positions) {
                    const auto item = std::find_if(items.begin(), items.end(), [&](const ShellDesktopItem& candidate) {
                        return PathsEqual(candidate.path, position.path);
                    });
                    if (item == items.end()) {
                        continue;
                    }
                    PCUITEMID_CHILD pidl = item->pidl;
                    POINT point = position.point;
                    if (SUCCEEDED(view->SelectAndPositionItems(1, &pidl, &point, SVSI_POSITIONITEM))) {
                        ++restored;
                    }
                }
                if (restored == positions.size()) {
                    return true;
                }
            }
            RefreshFolderView(view.Get());
        }
        Sleep(attempt < 10 ? 40 : 80);
    }
    errorMessage = L"文件已安全归还桌面，但 Explorer 未能恢复全部图标坐标。";
    return false;
}
