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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
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

void NotifyDesktopItemsCreated(
    const std::vector<std::wstring>& paths,
    bool flushLast) {
    struct Notification {
        std::wstring path;
        bool directory = false;
    };
    std::vector<Notification> notifications;
    notifications.reserve(paths.size());
    for (const std::wstring& path : paths) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            std::any_of(
                notifications.begin(),
                notifications.end(),
                [&](const Notification& value) { return PathsEqual(value.path, path); })) {
            continue;
        }
        notifications.push_back(Notification{
            path,
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    }
    for (size_t index = 0; index < notifications.size(); ++index) {
        const Notification& notification = notifications[index];
        const bool flush = flushLast && index + 1 == notifications.size();
        SHChangeNotify(
            notification.directory ? SHCNE_MKDIR : SHCNE_CREATE,
            SHCNF_PATHW | (flush ? SHCNF_FLUSH : SHCNF_FLUSHNOWAIT),
            notification.path.c_str(),
            nullptr);
    }
}

HRESULT GetDesktopFolderViewOnce(
    ComPtr<IFolderView>& folderView,
    HWND* viewWindow = nullptr) {
    folderView.Reset();
    if (viewWindow != nullptr) {
        *viewWindow = nullptr;
    }
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
    if (FAILED(result) || dispatch == nullptr) {
        return FAILED(result) ? result : E_NOINTERFACE;
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

HRESULT GetDesktopFolderView(
    ComPtr<IFolderView>& folderView,
    HWND* viewWindow = nullptr) {
    HRESULT result = E_FAIL;
    for (int attempt = 0; attempt < 6; ++attempt) {
        result = GetDesktopFolderViewOnce(folderView, viewWindow);
        if (SUCCEEDED(result) && folderView != nullptr &&
            (viewWindow == nullptr || *viewWindow != nullptr)) {
            return result;
        }
        if (attempt + 1 < 6) {
            Sleep(static_cast<DWORD>(20 * (attempt + 1)));
        }
    }
    return FAILED(result) ? result : E_NOINTERFACE;
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

std::wstring ItemDisplayName(
    IShellFolder* folder,
    PCUITEMID_CHILD item,
    const std::wstring& fallback) {
    STRRET displayName{};
    if (folder == nullptr ||
        FAILED(folder->GetDisplayNameOf(item, SHGDN_NORMAL, &displayName))) {
        return fallback;
    }
    std::vector<wchar_t> buffer(32768);
    if (FAILED(StrRetToBufW(
            &displayName,
            item,
            buffer.data(),
            static_cast<UINT>(buffer.size()))) ||
        buffer[0] == L'\0') {
        return fallback;
    }
    return buffer.data();
}

void ReadShellImageIdentity(
    IShellFolder* folder,
    PCUITEMID_CHILD child,
    int& systemImageIndex,
    int& overlayIndex) {
    systemImageIndex = -1;
    overlayIndex = 0;
    if (folder == nullptr || child == nullptr) {
        return;
    }
    ComPtr<IShellItem> shellItem;
    if (FAILED(SHCreateItemWithParent(
            nullptr,
            folder,
            child,
            IID_PPV_ARGS(shellItem.GetAddressOf()))) ||
        shellItem == nullptr) {
        return;
    }
    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    if (FAILED(SHGetIDListFromObject(
            shellItem.Get(), &absolutePidl)) ||
        absolutePidl == nullptr) {
        return;
    }
    SHFILEINFOW fileInfo{};
    constexpr UINT flags = SHGFI_PIDL | SHGFI_ICON | SHGFI_SYSICONINDEX |
        SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX;
    const DWORD_PTR result = SHGetFileInfoW(
        reinterpret_cast<LPCWSTR>(absolutePidl),
        0,
        &fileInfo,
        sizeof(fileInfo),
        flags);
    CoTaskMemFree(absolutePidl);
    if (result == 0) {
        return;
    }
    const unsigned int packed =
        static_cast<unsigned int>(fileInfo.iIcon);
    systemImageIndex = static_cast<int>(packed & 0x00FFFFFFU);
    overlayIndex = static_cast<int>((packed >> 24U) & 0xFFU);
    if (fileInfo.hIcon != nullptr) {
        DestroyIcon(fileInfo.hIcon);
    }
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
        if (SUCCEEDED(view->Item(index, &item.pidl)) &&
            item.pidl != nullptr &&
            !ILIsEmpty(item.pidl) &&
            ILIsEmpty(ILNext(item.pidl)) &&
            ItemPath(folder.Get(), item.pidl, item.path)) {
            items.push_back(std::move(item));
        }
    }
    return true;
}

bool ResolveDesktopViewItem(
    IFolderView* view,
    const std::wstring& path,
    ShellDesktopItem& resolved,
    std::wstring& errorMessage) {
    if (ParseDesktopViewItem(view, path, resolved)) {
        return true;
    }
    std::vector<ShellDesktopItem> items;
    if (!EnumerateDesktopItems(view, items, errorMessage)) {
        return false;
    }
    const auto found = std::find_if(
        items.begin(),
        items.end(),
        [&](const ShellDesktopItem& item) { return PathsEqual(item.path, path); });
    if (found == items.end()) {
        errorMessage = L"Explorer 桌面视图中没有找到该项目。";
        return false;
    }
    resolved.pidl = ILClone(found->pidl);
    if (resolved.pidl == nullptr) {
        errorMessage = L"无法复制 Explorer 桌面项目标识。";
        return false;
    }
    resolved.path = found->path;
    resolved.viewIndex = found->viewIndex;
    return true;
}

bool PositionViewItemAndConfirm(
    IFolderView* view,
    PCUITEMID_CHILD pidl,
    POINT requestedPoint,
    DWORD flags,
    POINT& confirmedPoint,
    std::wstring& errorMessage) {
    if (view == nullptr || pidl == nullptr) {
        errorMessage = L"Explorer 桌面项目标识无效。";
        return false;
    }
    if (FAILED(view->SelectAndPositionItems(1, &pidl, &requestedPoint, flags))) {
        errorMessage = L"Explorer 拒绝更新桌面项目的显示位置。";
        return false;
    }
    POINT previousPoint{};
    int stableSamples = 0;
    for (int attempt = 0; attempt < 18; ++attempt) {
        POINT confirmed{};
        if (SUCCEEDED(view->GetItemPosition(pidl, &confirmed))) {
            if (stableSamples > 0 &&
                confirmed.x == previousPoint.x && confirmed.y == previousPoint.y) {
                ++stableSamples;
            } else {
                previousPoint = confirmed;
                stableSamples = 1;
            }
            if (stableSamples >= 2) {
                confirmedPoint = confirmed;
                return true;
            }
        } else {
            stableSamples = 0;
        }
        Sleep(8);
    }
    errorMessage = L"Explorer 没有返回稳定的桌面项目显示位置。";
    return false;
}

bool CaptureViewSnapshotCore(
    DesktopViewSnapshot& snapshot,
    std::wstring& errorMessage) {
    errorMessage.clear();
    snapshot = {};
    ComPtr<IFolderView> view;
    HWND shellViewWindow = nullptr;
    const HRESULT viewResult =
        GetDesktopFolderView(view, &shellViewWindow);
    if (FAILED(viewResult) ||
        view == nullptr || shellViewWindow == nullptr) {
        errorMessage =
            L"无法连接 Explorer 桌面视图，未建立显示快照；stage=GetDesktopFolderView，HRESULT=" +
            std::to_wstring(static_cast<long long>(viewResult)) + L"。";
        return false;
    }
    const HWND listViewWindow = FindWindowExW(
        shellViewWindow, nullptr, L"SysListView32", nullptr);
    if (listViewWindow == nullptr ||
        IsWindow(listViewWindow) == FALSE ||
        !GetWindowRect(listViewWindow, &snapshot.screenRect)) {
        errorMessage =
            L"无法取得 Explorer 桌面图标窗口，未建立显示快照；stage=SysListView32。";
        snapshot = {};
        return false;
    }
    std::vector<ShellDesktopItem> shellItems;
    if (!EnumerateDesktopItems(view.Get(), shellItems, errorMessage)) {
        if (errorMessage.empty()) {
            errorMessage =
                L"无法枚举 Explorer 桌面项目；stage=SVGIO_ALLVIEW。";
        }
        snapshot = {};
        return false;
    }
    ComPtr<IShellFolder> folder;
    if (FAILED(view->GetFolder(IID_PPV_ARGS(&folder))) || folder == nullptr) {
        errorMessage =
            L"无法读取 Explorer 桌面项目名称，未建立显示快照；stage=IFolderView::GetFolder。";
        snapshot = {};
        return false;
    }
    HWND desktopHost = GetParent(shellViewWindow);
    wchar_t desktopHostClass[64]{};
    if (desktopHost == nullptr ||
        GetClassNameW(
            desktopHost,
            desktopHostClass,
            ARRAYSIZE(desktopHostClass)) == 0 ||
        (wcscmp(desktopHostClass, L"WorkerW") != 0 &&
         wcscmp(desktopHostClass, L"Progman") != 0)) {
        desktopHost = GetAncestor(listViewWindow, GA_ROOT);
    }
    snapshot.desktopHost = desktopHost;
    snapshot.shellViewWindow = shellViewWindow;
    snapshot.listViewWindow = listViewWindow;
    ComPtr<IFolderView2> view2;
    if (SUCCEEDED(view.As(&view2)) && view2 != nullptr) {
        view2->GetCurrentFolderFlags(&snapshot.viewFlags);
        FOLDERVIEWMODE viewMode = FVM_AUTO;
        int viewIconSize = 0;
        if (SUCCEEDED(view2->GetViewModeAndIconSize(
                &viewMode, &viewIconSize)) &&
            viewIconSize >= 16 && viewIconSize <= 256) {
            snapshot.viewIconSize = viewIconSize;
        }
    }
    snapshot.items.reserve(shellItems.size());
    for (const ShellDesktopItem& shellItem : shellItems) {
        POINT viewPoint{};
        if (FAILED(view->GetItemPosition(shellItem.pidl, &viewPoint))) {
            continue;
        }
        POINT screenPoint = viewPoint;
        if (ClientToScreen(listViewWindow, &screenPoint) == FALSE) {
            continue;
        }
        int systemImageIndex = -1;
        int overlayIndex = 0;
        ReadShellImageIdentity(
            folder.Get(),
            shellItem.pidl,
            systemImageIndex,
            overlayIndex);
        const UINT shellChildPidlSize =
            ILGetSize(shellItem.pidl);
        if (shellChildPidlSize <
            sizeof(USHORT) * 2U) {
            continue;
        }
        std::vector<BYTE> shellChildPidl(
            shellChildPidlSize);
        std::memcpy(
            shellChildPidl.data(),
            shellItem.pidl,
            shellChildPidlSize);
        snapshot.items.push_back(DesktopViewItem{
            shellItem.path,
            ItemDisplayName(
                folder.Get(), shellItem.pidl, FileName(shellItem.path)),
            viewPoint,
            screenPoint,
            shellItem.viewIndex,
            systemImageIndex,
            overlayIndex,
            std::move(shellChildPidl)});
    }
    if (!shellItems.empty() && snapshot.items.empty()) {
        errorMessage = L"Explorer 未返回任何可定位桌面项目，未建立显示快照。";
        snapshot = {};
        return false;
    }
    if (snapshot.desktopHost == nullptr ||
        IsWindow(snapshot.desktopHost) == FALSE) {
        errorMessage =
            L"Explorer 桌面根窗口在建立快照时已失效。";
        snapshot = {};
        return false;
    }
    return true;
}

constexpr auto kDesktopSnapshotTimeout = std::chrono::seconds(5);
std::atomic<bool> gDesktopSnapshotInFlight{false};

struct DesktopSnapshotState {
    std::mutex mutex;
    std::condition_variable completed;
    std::atomic<DWORD> threadId{0};
    bool done = false;
    bool succeeded = false;
    DesktopViewSnapshot snapshot;
    std::wstring errorMessage;
};

}  // namespace

bool DesktopLayout::CaptureViewSnapshot(
    DesktopViewSnapshot& snapshot,
    std::wstring& errorMessage) const {
    snapshot = {};
    errorMessage.clear();
    if (gDesktopSnapshotInFlight.exchange(true)) {
        errorMessage =
            L"Explorer 桌面快照仍在等待上一轮有界调用结束，未重复发起接管。";
        return false;
    }

    const auto state = std::make_shared<DesktopSnapshotState>();
    try {
        std::thread([state]() {
            state->threadId.store(GetCurrentThreadId());
            const HRESULT initializeResult = OleInitialize(nullptr);
            const bool initialized = SUCCEEDED(initializeResult);
            const HRESULT cancellationResult = initialized
                ? CoEnableCallCancellation(nullptr)
                : initializeResult;
            DesktopViewSnapshot captured;
            std::wstring captureError;
            const bool succeeded = initialized &&
                CaptureViewSnapshotCore(captured, captureError);
            if (!initialized) {
                captureError =
                    L"无法初始化有界 Explorer 桌面快照线程，HRESULT=" +
                    std::to_wstring(
                        static_cast<long long>(initializeResult)) + L"。";
            }
            if (SUCCEEDED(cancellationResult)) {
                CoDisableCallCancellation(nullptr);
            }
            if (initialized) {
                OleUninitialize();
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->succeeded = succeeded;
                state->snapshot = std::move(captured);
                state->errorMessage = std::move(captureError);
                state->done = true;
            }
            gDesktopSnapshotInFlight.store(false);
            state->completed.notify_one();
        }).detach();
    } catch (...) {
        gDesktopSnapshotInFlight.store(false);
        errorMessage = L"无法启动有界 Explorer 桌面快照线程。";
        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->completed.wait_for(
            lock, kDesktopSnapshotTimeout,
            [&]() { return state->done; })) {
        const DWORD threadId = state->threadId.load();
        if (threadId != 0) {
            CoCancelCall(threadId, 0);
        }
        errorMessage =
            L"Explorer 桌面快照超过 5 秒有界门，已取消本次显示接管；stage=bounded-timeout。";
        return false;
    }
    snapshot = std::move(state->snapshot);
    errorMessage = std::move(state->errorMessage);
    return state->succeeded;
}

bool DesktopLayout::CaptureViewFlags(DWORD& flags, std::wstring& errorMessage) const {
    errorMessage.clear();
    flags = 0;
    ComPtr<IFolderView> view;
    if (FAILED(GetDesktopFolderView(view))) {
        errorMessage = L"无法连接 Explorer 桌面视图，未读取桌面对齐设置。";
        return false;
    }
    ComPtr<IFolderView2> view2;
    if (FAILED(view.As(&view2)) || view2 == nullptr ||
        FAILED(view2->GetCurrentFolderFlags(&flags))) {
        errorMessage = L"Explorer 桌面视图不支持读取对齐设置。";
        return false;
    }
    return true;
}

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
        errorMessage = L"无法连接 Explorer 桌面视图，未读取该项目坐标。";
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
            errorMessage = L"Explorer 没有返回该桌面项目的坐标。";
            return false;
        }
    }
    errorMessage = L"Explorer 桌面视图中没有找到该项目。";
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

bool DesktopLayout::RestorePosition(
    const std::wstring& path,
    POINT viewPoint,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    ComPtr<IFolderView> view;
    if (FAILED(GetDesktopFolderView(view))) {
        errorMessage = L"无法连接 Explorer 桌面视图，未恢复该项目。";
        return false;
    }
    ShellDesktopItem item;
    if (!ResolveDesktopViewItem(view.Get(), path, item, errorMessage)) {
        return false;
    }
    POINT confirmedPoint{};
    return PositionViewItemAndConfirm(
        view.Get(), item.pidl, viewPoint,
        SVSI_POSITIONITEM | SVSI_NOSTATECHANGE,
        confirmedPoint, errorMessage);
}

bool DesktopLayout::RestoreScreenPositionOnce(
    const std::wstring& path,
    POINT screenPoint,
    POINT& restoredPoint,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    ComPtr<IFolderView> view;
    HWND viewHwnd = nullptr;
    if (FAILED(GetDesktopFolderView(view, &viewHwnd))) {
        errorMessage = L"无法连接 Explorer 桌面视图，未放置该项目。";
        return false;
    }
    const HWND listView = viewHwnd == nullptr
        ? nullptr
        : FindWindowExW(viewHwnd, nullptr, L"SysListView32", nullptr);
    POINT expectedViewPoint = screenPoint;
    if (listView == nullptr || ScreenToClient(listView, &expectedViewPoint) == FALSE) {
        errorMessage = L"无法把鼠标释放点转换为 Explorer 桌面坐标。";
        return false;
    }
    ShellDesktopItem item;
    if (!ResolveDesktopViewItem(view.Get(), path, item, errorMessage)) {
        return false;
    }
    if (!PositionViewItemAndConfirm(
            view.Get(), item.pidl, expectedViewPoint,
            SVSI_POSITIONITEM | SVSI_NOSTATECHANGE,
            restoredPoint, errorMessage)) {
        return false;
    }
    return true;
}

bool DesktopLayout::PositionScreenItemsOnce(
    const std::vector<DesktopPosition>& screenPositions,
    std::vector<DesktopPosition>& confirmedScreenPositions,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    confirmedScreenPositions.clear();
    if (screenPositions.empty()) {
        return true;
    }

    ComPtr<IFolderView> view;
    HWND viewHwnd = nullptr;
    if (FAILED(GetDesktopFolderView(view, &viewHwnd)) || view == nullptr) {
        errorMessage = L"无法连接 Explorer 桌面视图，未移动桌面项目。";
        return false;
    }
    const HWND listView = viewHwnd == nullptr
        ? nullptr
        : FindWindowExW(viewHwnd, nullptr, L"SysListView32", nullptr);
    if (listView == nullptr || IsWindow(listView) == FALSE) {
        errorMessage = L"无法取得 Explorer 桌面视图窗口，未移动桌面项目。";
        return false;
    }

    std::vector<ShellDesktopItem> items;
    if (!EnumerateDesktopItems(view.Get(), items, errorMessage)) {
        return false;
    }
    std::vector<PCUITEMID_CHILD> pidls;
    std::vector<POINT> viewPoints;
    pidls.reserve(screenPositions.size());
    viewPoints.reserve(screenPositions.size());
    for (const DesktopPosition& position : screenPositions) {
        const auto item = std::find_if(
            items.begin(), items.end(),
            [&](const ShellDesktopItem& candidate) {
                return PathsEqual(candidate.path, position.path);
            });
        if (item == items.end()) {
            errorMessage = L"Explorer 桌面视图中没有找到全部待移动项目。";
            return false;
        }
        POINT viewPoint = position.point;
        if (ScreenToClient(listView, &viewPoint) == FALSE) {
            errorMessage = L"无法把桌面释放点转换为 Explorer 视图坐标。";
            return false;
        }
        pidls.push_back(item->pidl);
        viewPoints.push_back(viewPoint);
    }

    if (FAILED(view->SelectAndPositionItems(
            static_cast<UINT>(pidls.size()),
            pidls.data(),
            viewPoints.data(),
            SVSI_POSITIONITEM | SVSI_NOSTATECHANGE))) {
        errorMessage = L"Explorer 拒绝批量更新桌面项目位置。";
        return false;
    }

    confirmedScreenPositions.reserve(screenPositions.size());
    for (size_t index = 0; index < pidls.size(); ++index) {
        POINT confirmed = viewPoints[index];
        POINT actual{};
        if (SUCCEEDED(view->GetItemPosition(pidls[index], &actual))) {
            confirmed = actual;
        }
        if (ClientToScreen(listView, &confirmed) == FALSE) {
            confirmed = screenPositions[index].point;
        }
        confirmedScreenPositions.push_back(
            DesktopPosition{screenPositions[index].path, confirmed});
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
                SVSI_POSITIONITEM | SVSI_NOSTATECHANGE;
            if (!parsedPositionIssued && exactViewPointKnown) {
                ShellDesktopItem parsedItem;
                if (ParseDesktopViewItem(view.Get(), path, parsedItem)) {
                    if (isCancelled()) {
                        errorMessage = L"桌面定位已取消。";
                        return false;
                    }
                    PCUITEMID_CHILD pidl = parsedItem.pidl;
                    POINT targetPoint = exactViewPoint;
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
                        PCUITEMID_CHILD pidl = visibleItem->pidl;
                        POINT targetPoint = exactViewPoint;
                        if (SUCCEEDED(view->SelectAndPositionItems(
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
                                    afterRestore.x != confirmed.x ||
                                    afterRestore.y != confirmed.y) {
                                    errorMessage =
                                        L"Explorer 定位后改变了图标所在网格位置。";
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
    errorMessage = L"显示归属已切换到桌面，但 Explorer 未能把图标放到鼠标释放位置；原件路径没有改变。";
    return false;
}

bool DesktopLayout::RestorePositions(
    const std::vector<DesktopPosition>& positions,
    std::wstring& errorMessage) const {
    std::vector<std::wstring> requiredPaths;
    requiredPaths.reserve(positions.size());
    for (const DesktopPosition& position : positions) {
        if (std::none_of(
                requiredPaths.begin(),
                requiredPaths.end(),
                [&](const std::wstring& path) { return PathsEqual(path, position.path); })) {
            requiredPaths.push_back(position.path);
        }
    }
    return RestorePositions(positions, requiredPaths, errorMessage, false);
}

bool DesktopLayout::RestorePositions(
    const std::vector<DesktopPosition>& positions,
    const std::vector<std::wstring>& requiredPaths,
    std::wstring& errorMessage,
    bool notifyRequiredPaths) const {
    errorMessage.clear();
    if (positions.empty() && requiredPaths.empty()) {
        return true;
    }

    std::vector<DesktopPosition> uniquePositions;
    uniquePositions.reserve(positions.size());
    for (const DesktopPosition& position : positions) {
        const auto existing = std::find_if(
            uniquePositions.begin(),
            uniquePositions.end(),
            [&](const DesktopPosition& value) { return PathsEqual(value.path, position.path); });
        if (existing == uniquePositions.end()) {
            uniquePositions.push_back(position);
        } else {
            *existing = position;
        }
    }
    std::vector<std::wstring> uniqueRequiredPaths;
    uniqueRequiredPaths.reserve(requiredPaths.size());
    for (const std::wstring& path : requiredPaths) {
        if (std::none_of(
                uniqueRequiredPaths.begin(),
                uniqueRequiredPaths.end(),
                [&](const std::wstring& value) { return PathsEqual(value, path); })) {
            uniqueRequiredPaths.push_back(path);
        }
    }

    if (notifyRequiredPaths) {
        NotifyDesktopItemsCreated(uniqueRequiredPaths, false);
    }
    constexpr ULONGLONG kTotalTimeoutMilliseconds = 1500;
    const ULONGLONG startedAt = GetTickCount64();
    bool refreshedView = false;
    for (;;) {
        ComPtr<IFolderView> view;
        if (SUCCEEDED(GetDesktopFolderView(view))) {
            std::vector<ShellDesktopItem> items;
            std::wstring enumerateError;
            if (EnumerateDesktopItems(view.Get(), items, enumerateError)) {
                const bool allRequiredVisible = std::all_of(
                    uniqueRequiredPaths.begin(),
                    uniqueRequiredPaths.end(),
                    [&](const std::wstring& path) {
                        return std::any_of(
                            items.begin(),
                            items.end(),
                            [&](const ShellDesktopItem& item) { return PathsEqual(item.path, path); });
                    });
                if (allRequiredVisible) {
                    std::vector<PCUITEMID_CHILD> pidls;
                    std::vector<POINT> points;
                    pidls.reserve(uniquePositions.size());
                    points.reserve(uniquePositions.size());
                    for (const DesktopPosition& position : uniquePositions) {
                        const auto item = std::find_if(
                            items.begin(),
                            items.end(),
                            [&](const ShellDesktopItem& candidate) {
                                return PathsEqual(candidate.path, position.path);
                            });
                        if (item != items.end()) {
                            pidls.push_back(item->pidl);
                            points.push_back(position.point);
                        }
                    }
                    if (pidls.empty()) {
                        return true;
                    }
                    ULONGLONG lastPositionedAt = 0;
                    int stableSamples = 0;
                    while (GetTickCount64() - startedAt < kTotalTimeoutMilliseconds) {
                        const ULONGLONG now = GetTickCount64();
                        if (lastPositionedAt == 0 || now - lastPositionedAt >= 120) {
                            std::vector<PCUITEMID_CHILD> displacedPidls;
                            std::vector<POINT> displacedPoints;
                            displacedPidls.reserve(pidls.size());
                            displacedPoints.reserve(points.size());
                            for (size_t index = 0; index < pidls.size(); ++index) {
                                POINT actual{};
                                if (FAILED(view->GetItemPosition(pidls[index], &actual)) ||
                                    actual.x != points[index].x || actual.y != points[index].y) {
                                    displacedPidls.push_back(pidls[index]);
                                    displacedPoints.push_back(points[index]);
                                }
                            }
                            if (!displacedPidls.empty() &&
                                FAILED(view->SelectAndPositionItems(
                                    static_cast<UINT>(displacedPidls.size()),
                                    displacedPidls.data(),
                                    displacedPoints.data(),
                                    SVSI_POSITIONITEM))) {
                                stableSamples = 0;
                            }
                            lastPositionedAt = now;
                        }
                        bool exact = true;
                        for (size_t index = 0; index < pidls.size(); ++index) {
                            POINT actual{};
                            if (FAILED(view->GetItemPosition(pidls[index], &actual)) ||
                                actual.x != points[index].x || actual.y != points[index].y) {
                                exact = false;
                                break;
                            }
                        }
                        stableSamples = exact ? stableSamples + 1 : 0;
                        if (stableSamples >= 2) {
                            return true;
                        }
                        const ULONGLONG elapsed = GetTickCount64() - startedAt;
                        if (elapsed >= kTotalTimeoutMilliseconds) {
                            break;
                        }
                        Sleep(static_cast<DWORD>((std::min)(
                            static_cast<ULONGLONG>(8),
                            kTotalTimeoutMilliseconds - elapsed)));
                    }
                    errorMessage = L"文件已安全归还桌面，但 Explorer 没有稳定保持启动前的图标坐标。";
                    return false;
                }
            }
        }
        const ULONGLONG elapsed = GetTickCount64() - startedAt;
        if (notifyRequiredPaths && !refreshedView && elapsed >= 160) {
            NotifyDesktopItemsCreated(uniqueRequiredPaths, true);
            refreshedView = true;
        }
        if (elapsed >= kTotalTimeoutMilliseconds) {
            break;
        }
        Sleep(elapsed < 160 ? 8 : 32);
    }
    errorMessage = L"文件已安全归还桌面，但 Explorer 未在限定时间内显示全部归还项目。";
    return false;
}
