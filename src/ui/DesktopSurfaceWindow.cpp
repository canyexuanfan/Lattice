#include "ui/DesktopSurfaceWindow.h"

#include <CommCtrl.h>
#include <d2d1helper.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "shell/ShellDragDrop.h"
#include "ui/MessageDialog.h"

namespace {

constexpr wchar_t kDesktopSurfaceClassName[] =
    L"Lattice.DesktopSurfaceWindow";
constexpr UINT kIconReadyMessage = WM_APP + 41;
constexpr UINT kFinishRenameMessage = WM_APP + 42;
constexpr UINT kCancelRenameMessage = WM_APP + 43;
constexpr UINT kBeginRenameMessage = WM_APP + 44;
constexpr UINT_PTR kRenameTimerId = 0x52454E41;
constexpr UINT_PTR kRenameSubclassId = 1;
constexpr DWORD kListViewQueryTimeoutMilliseconds = 50;

bool IdentitiesEqual(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
               left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

struct DesktopLabelStyle {
    std::wstring faceName = L"Microsoft YaHei UI";
    FLOAT size = 12.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
};

DesktopLabelStyle ReadDesktopLabelStyle() {
    LOGFONTW iconFont{};
    DesktopLabelStyle result;
    if (SystemParametersInfoW(
            SPI_GETICONTITLELOGFONT,
            sizeof(iconFont),
            &iconFont,
            0) == FALSE) {
        return result;
    }
    if (iconFont.lfFaceName[0] != L'\0') {
        result.faceName = iconFont.lfFaceName;
    }
    const LONG height =
        iconFont.lfHeight < 0 ? -iconFont.lfHeight : iconFont.lfHeight;
    result.size = static_cast<FLOAT>(std::clamp<LONG>(height, 8, 32));
    result.weight = static_cast<DWRITE_FONT_WEIGHT>(
        std::clamp<LONG>(iconFont.lfWeight, 100, 900));
    result.style = iconFont.lfItalic != FALSE
        ? DWRITE_FONT_STYLE_ITALIC
        : DWRITE_FONT_STYLE_NORMAL;
    return result;
}

}  // namespace

DesktopSurfaceWindow* DesktopSurfaceWindow::keyboardHookOwner_ = nullptr;

class DesktopSurfaceDropTarget final : public IDropTarget {
public:
    DesktopSurfaceDropTarget(
        DesktopSurfaceWindow* owner,
        IDropTarget* explorerTarget)
        : owner_(owner), explorerTarget_(explorerTarget) {
        CoCreateInstance(
            CLSID_DragDropHelper,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dragImageHelper_.GetAddressOf()));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject* dataObject,
        DWORD keyState,
        POINTL point,
        DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        internalDrag_ = owner_ != nullptr && owner_->internalDragActive_;
#ifndef NDEBUG
        if (internalDrag_) {
            owner_->internalDropStage_ =
                DesktopSurfaceWindow::InternalDropStage::DragEntered;
        }
#endif
        HRESULT routedResult = S_OK;
        if (internalDrag_) {
            *effect = (*effect & DROPEFFECT_MOVE) != 0
                ? DROPEFFECT_MOVE
                : DROPEFFECT_NONE;
        } else if (explorerTarget_ != nullptr) {
            routedResult = explorerTarget_->DragEnter(
                dataObject, keyState, point, effect);
        } else {
            *effect = DROPEFFECT_NONE;
            routedResult = E_FAIL;
        }
        POINT screenPoint{point.x, point.y};
        GetPhysicalCursorPos(&screenPoint);
        if (dragImageHelper_ != nullptr && owner_ != nullptr) {
            dragImageHelper_->DragEnter(
                owner_->Window(),
                dataObject,
                &screenPoint,
                *effect);
        }
        return routedResult;
    }

    HRESULT STDMETHODCALLTYPE DragOver(
        DWORD keyState,
        POINTL point,
        DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        HRESULT routedResult = S_OK;
        if (internalDrag_) {
            *effect = owner_ != nullptr && owner_->internalDragActive_
                ? DROPEFFECT_MOVE
                : DROPEFFECT_NONE;
        } else if (explorerTarget_ != nullptr) {
            routedResult = explorerTarget_->DragOver(
                keyState, point, effect);
        } else {
            *effect = DROPEFFECT_NONE;
            routedResult = E_FAIL;
        }
        POINT screenPoint{point.x, point.y};
        GetPhysicalCursorPos(&screenPoint);
        if (dragImageHelper_ != nullptr) {
            dragImageHelper_->DragOver(&screenPoint, *effect);
        }
        return routedResult;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        const bool wasInternal = internalDrag_;
        internalDrag_ = false;
        if (dragImageHelper_ != nullptr) {
            dragImageHelper_->DragLeave();
        }
        if (!wasInternal) {
            return explorerTarget_ != nullptr
                ? explorerTarget_->DragLeave()
                : E_FAIL;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject* dataObject,
        DWORD keyState,
        POINTL point,
        DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        POINT screenPoint{point.x, point.y};
        GetPhysicalCursorPos(&screenPoint);
        if (dragImageHelper_ != nullptr) {
            dragImageHelper_->Drop(
                dataObject, &screenPoint, *effect);
        }
        if (!internalDrag_) {
            return explorerTarget_ != nullptr
                ? explorerTarget_->Drop(dataObject, keyState, point, effect)
                : E_FAIL;
        }
        internalDrag_ = false;
#ifndef NDEBUG
        if (owner_ != nullptr) {
            owner_->internalDropStage_ =
                DesktopSurfaceWindow::InternalDropStage::DropReceived;
        }
#endif
        const bool positioned = owner_ != nullptr &&
            owner_->CommitInternalDesktopDrop(screenPoint);
        *effect = positioned ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    DesktopSurfaceWindow* owner_ = nullptr;
    Microsoft::WRL::ComPtr<IDropTarget> explorerTarget_;
    Microsoft::WRL::ComPtr<IDropTargetHelper> dragImageHelper_;
    bool internalDrag_ = false;
};

DesktopSurfaceWindow::DesktopSurfaceWindow(HINSTANCE instance)
    : instance_(instance) {
}

DesktopSurfaceWindow::~DesktopSurfaceWindow() {
    Close();
}

bool DesktopSurfaceWindow::IdentityLess::operator()(
    const std::wstring& left,
    const std::wstring& right) const noexcept {
    const int comparison = CompareStringOrdinal(
        left.c_str(), -1, right.c_str(), -1, TRUE);
    if (comparison == CSTR_LESS_THAN) {
        return true;
    }
    if (comparison == CSTR_GREATER_THAN ||
        comparison == CSTR_EQUAL) {
        return false;
    }
    return left < right;
}

bool DesktopSurfaceWindow::Create(
    const std::vector<std::wstring>& assignedIdentities,
    std::wstring& errorMessage) {
    Close();
    assignedIdentities_ = assignedIdentities;
    DesktopLayout layout;
    if (!layout.CaptureViewSnapshot(snapshot_, errorMessage)) {
        return false;
    }
    HWND selectionViewWindow = nullptr;
    if (!layout.AcquireFolderViewOnce(
            explorerFolderView_.ReleaseAndGetAddressOf(),
            explorerShellView_.ReleaseAndGetAddressOf(),
            selectionViewWindow,
            errorMessage) ||
        selectionViewWindow != snapshot_.shellViewWindow) {
        explorerFolderView_.Reset();
        if (errorMessage.empty()) {
            errorMessage =
                L"Explorer桌面视图在建立显示接管时已重建。";
        }
        explorerShellView_.Reset();
        return false;
    }
    const HRESULT selectionFolderResult =
        explorerFolderView_->GetFolder(
            IID_PPV_ARGS(
                explorerDesktopFolder_.ReleaseAndGetAddressOf()));
    if (FAILED(selectionFolderResult) ||
        explorerDesktopFolder_ == nullptr) {
        errorMessage =
            L"无法连接Explorer桌面选择父文件夹，HRESULT=" +
            std::to_wstring(
                static_cast<long long>(selectionFolderResult)) + L"。";
        explorerFolderView_.Reset();
        explorerShellView_.Reset();
        return false;
    }
    InitializeListViewQueryAccess();
    UpdateViewMetrics();
    RebuildVisibleItems();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kDesktopSurfaceClassName;
    if (RegisterClassExW(&windowClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        errorMessage = L"无法注册 Lattice 桌面显示接管窗口。";
        return false;
    }
    const int width =
        snapshot_.screenRect.right - snapshot_.screenRect.left;
    const int height =
        snapshot_.screenRect.bottom - snapshot_.screenRect.top;
    POINT parentOrigin{
        snapshot_.screenRect.left,
        snapshot_.screenRect.top};
    if (ScreenToClient(
            snapshot_.desktopHost,
            &parentOrigin) == FALSE) {
        errorMessage =
            L"无法把 Explorer 图标区域映射到桌面父窗口。";
        return false;
    }
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kDesktopSurfaceClassName,
        L"Lattice desktop display takeover",
        WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        snapshot_.screenRect.left,
        snapshot_.screenRect.top,
        width,
        height,
        nullptr,
        nullptr,
        instance_,
        this);
    if (hwnd_ == nullptr) {
        errorMessage = L"无法创建 Lattice 桌面显示接管窗口。";
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const HWND previousParent = SetParent(hwnd_, snapshot_.desktopHost);
    if ((previousParent == nullptr && GetLastError() != ERROR_SUCCESS) ||
        GetAncestor(hwnd_, GA_PARENT) != snapshot_.desktopHost ||
        (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_POPUP) == 0 ||
        (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CHILD) != 0) {
        errorMessage =
            L"无法按 DeskGo 模型把 Lattice 桌面显示面挂接到桌面根。";
        Close();
        return false;
    }
    SetWindowPos(
        hwnd_, HWND_TOP, parentOrigin.x, parentOrigin.y, width, height,
        SWP_NOACTIVATE);
    RebuildInteractionRects();
    if (!d2d_.Initialize(hwnd_)) {
        errorMessage = L"无法初始化 Lattice 桌面显示接管渲染器。";
        Close();
        return false;
    }
    ConfigurePixelRenderTarget();
    Microsoft::WRL::ComPtr<IShellFolder> desktopFolder;
    Microsoft::WRL::ComPtr<IDropTarget> explorerDropTarget;
    HRESULT dropResult = SHGetDesktopFolder(
        desktopFolder.GetAddressOf());
    if (SUCCEEDED(dropResult) && desktopFolder != nullptr) {
        dropResult = desktopFolder->CreateViewObject(
            hwnd_, IID_PPV_ARGS(explorerDropTarget.GetAddressOf()));
    }
    if (SUCCEEDED(dropResult) && explorerDropTarget != nullptr) {
        desktopDropTarget_.Attach(new (std::nothrow) DesktopSurfaceDropTarget(
            this, explorerDropTarget.Get()));
        if (desktopDropTarget_ == nullptr) {
            dropResult = E_OUTOFMEMORY;
        }
    }
    if (FAILED(dropResult) || desktopDropTarget_ == nullptr ||
        FAILED(RegisterDragDrop(hwnd_, desktopDropTarget_.Get()))) {
        errorMessage =
            L"无法注册 Explorer 原生桌面拖放目标。";
        Close();
        return false;
    }
    dropTargetRegistered_ = true;
    iconCache_.SetCapacity(std::max<size_t>(
        256, visibleItems_.size() + 32));
    iconCache_.SetInvalidateCallback([this]() {
        const HWND window = hwnd_;
        if (window != nullptr && IsWindow(window) != FALSE) {
            PostMessageW(window, kIconReadyMessage, 0, 0);
        }
    });
    for (const DesktopViewItem& item : visibleItems_) {
        iconCache_.PreloadShellIcon(
            item.path,
            item.systemImageIndex,
            item.overlayIndex,
            iconSize_);
    }
    if (!wallpaper_.Refresh(
            hwnd_, d2d_.Target(), width, height, false)) {
        errorMessage =
            L"无法创建与当前桌面一致的无模糊壁纸快照。";
        Close();
        return false;
    }
    if (!InstallKeyboardHook()) {
        errorMessage =
            L"无法建立桌面F2键盘路由。";
        Close();
        return false;
    }
    MaintainDesktopLayer();
    return true;
}

void DesktopSurfaceWindow::Show() {
    if (hwnd_ == nullptr) {
        return;
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    MaintainDesktopLayer();
    RedrawWindow(
        hwnd_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
}

void DesktopSurfaceWindow::Hide() {
    if (hwnd_ != nullptr) {
        desktopKeyboardSelectionArmed_ = false;
        CancelPendingRename();
        FinishRename(false);
        CancelPointerCapture();
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void DesktopSurfaceWindow::UpdateAssignedIdentities(
    const std::vector<std::wstring>& assignedIdentities) {
    assignedIdentities_ = assignedIdentities;
    RebuildVisibleItems();
    for (const DesktopViewItem& item : visibleItems_) {
        iconCache_.PreloadShellIcon(
            item.path,
            item.systemImageIndex,
            item.overlayIndex,
            iconSize_);
    }
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DesktopSurfaceWindow::UpdateDisplayPositions(
    const std::vector<DesktopPosition>& viewPositions) {
    for (const PositionOverride& value : positionOverrides_) {
        SetItemScreenPoint(value.identity, value.nativeScreenPoint);
    }
    positionOverrides_.clear();
    for (const DesktopPosition& position : viewPositions) {
        const auto item = std::find_if(
            snapshot_.items.begin(), snapshot_.items.end(),
            [&](const DesktopViewItem& value) {
                return IdentitiesEqual(value.path, position.path);
            });
        if (item == snapshot_.items.end()) {
            continue;
        }
        POINT screenPoint = position.point;
        if (snapshot_.listViewWindow == nullptr ||
            IsWindow(snapshot_.listViewWindow) == FALSE ||
            ClientToScreen(
                snapshot_.listViewWindow,
                &screenPoint) == FALSE) {
            continue;
        }
        const POINT nativeScreenPoint = item->screenPoint;
        positionOverrides_.push_back(PositionOverride{
            item->path,
            screenPoint,
            nativeScreenPoint,
            position.point});
        SetItemScreenPoint(item->path, screenPoint);
    }
    RebuildVisibleItems();
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DesktopSurfaceWindow::SetDisplayPositionCommitHandler(
    DisplayPositionCommitHandler handler) {
    displayPositionCommitHandler_ = std::move(handler);
}

void DesktopSurfaceWindow::SetRenameCommitHandler(
    RenameCommitHandler handler) {
    renameCommitHandler_ = std::move(handler);
}

void DesktopSurfaceWindow::PresentUnassignedItemAt(
    const std::wstring& identity,
    POINT screenPoint) {
    const auto existing = std::find_if(
        positionOverrides_.begin(), positionOverrides_.end(),
        [&](const PositionOverride& value) {
            return IdentitiesEqual(value.identity, identity);
        });
    if (existing == positionOverrides_.end()) {
        POINT viewPoint = screenPoint;
        if (snapshot_.listViewWindow != nullptr &&
            IsWindow(snapshot_.listViewWindow) != FALSE) {
            ScreenToClient(snapshot_.listViewWindow, &viewPoint);
        }
        POINT nativeScreenPoint = screenPoint;
        const auto item = std::find_if(
            snapshot_.items.begin(), snapshot_.items.end(),
            [&](const DesktopViewItem& value) {
                return IdentitiesEqual(value.path, identity);
            });
        if (item != snapshot_.items.end()) {
            nativeScreenPoint = item->screenPoint;
        }
        positionOverrides_.push_back(PositionOverride{
            identity, screenPoint, nativeScreenPoint, viewPoint});
    } else {
        existing->screenPoint = screenPoint;
        existing->viewPoint = screenPoint;
        if (snapshot_.listViewWindow != nullptr &&
            IsWindow(snapshot_.listViewWindow) != FALSE) {
            ScreenToClient(
                snapshot_.listViewWindow,
                &existing->viewPoint);
        }
    }
    SetItemScreenPoint(identity, screenPoint);
    RebuildVisibleItems();
    if (hwnd_ != nullptr) {
        RedrawWindow(
            hwnd_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

void DesktopSurfaceWindow::ConfirmUnassignedItemAt(
    const std::wstring& identity,
    POINT screenPoint) {
    positionOverrides_.erase(
        std::remove_if(
            positionOverrides_.begin(), positionOverrides_.end(),
            [&](const PositionOverride& value) {
                return IdentitiesEqual(value.identity, identity);
            }),
        positionOverrides_.end());
    SetItemScreenPoint(identity, screenPoint);
    RebuildVisibleItems();
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DesktopSurfaceWindow::Close() {
    RemoveKeyboardHook();
    CancelPendingRename();
    FinishRename(false);
    CancelPointerCapture();
    iconCache_.SetInvalidateCallback(nullptr);
    if (dropTargetRegistered_ && hwnd_ != nullptr) {
        RevokeDragDrop(hwnd_);
    }
    dropTargetRegistered_ = false;
    desktopDropTarget_.Reset();
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) {
        DestroyWindow(hwnd_);
    }
    ReleaseListViewQueryAccess();
    explorerDesktopFolder_.Reset();
    explorerShellView_.Reset();
    explorerFolderView_.Reset();
    lastExplorerSelectionSyncResult_ = E_PENDING;
    lastExplorerSelectionSyncStage_ = 0;
    hwnd_ = nullptr;
    visibleItems_.clear();
    visibleInteractionRects_.clear();
    positionOverrides_.clear();
    snapshot_ = {};
    hoverIndex_ = -1;
    selectedIdentities_.clear();
    desktopKeyboardSelectionArmed_ = false;
    EndInternalDragSession();
    ResetPointerGesture();
}

bool DesktopSurfaceWindow::Refresh(
    std::wstring& errorMessage,
    bool refreshWallpaper) {
    if (hwnd_ == nullptr) {
        errorMessage = L"Lattice 桌面显示接管尚未创建。";
        return false;
    }
    if (renameEdit_ != nullptr) {
        FinishRename(true);
        if (renameEdit_ != nullptr) {
            errorMessage =
                L"当前重命名仍需用户修正，已暂缓刷新桌面项目。";
            return false;
        }
    }
    CancelPendingRename();
    CancelPointerCapture();
    DesktopViewSnapshot next;
    DesktopLayout layout;
    if (!layout.CaptureViewSnapshot(next, errorMessage)) {
        return false;
    }
    Microsoft::WRL::ComPtr<IFolderView> nextFolderView;
    Microsoft::WRL::ComPtr<IShellView> nextShellView;
    HWND nextSelectionViewWindow = nullptr;
    if (!layout.AcquireFolderViewOnce(
            nextFolderView.GetAddressOf(),
            nextShellView.GetAddressOf(),
            nextSelectionViewWindow,
            errorMessage) ||
        nextSelectionViewWindow != next.shellViewWindow) {
        if (errorMessage.empty()) {
            errorMessage =
                L"Explorer桌面视图在刷新显示接管时已重建。";
        }
        return false;
    }
    Microsoft::WRL::ComPtr<IShellFolder> nextDesktopFolder;
    const HRESULT nextFolderResult = nextFolderView->GetFolder(
        IID_PPV_ARGS(nextDesktopFolder.GetAddressOf()));
    if (FAILED(nextFolderResult) || nextDesktopFolder == nullptr) {
        errorMessage =
            L"无法刷新Explorer桌面选择父文件夹，HRESULT=" +
            std::to_wstring(
                static_cast<long long>(nextFolderResult)) + L"。";
        return false;
    }
    ReleaseListViewQueryAccess();
    snapshot_ = std::move(next);
    explorerFolderView_ = std::move(nextFolderView);
    explorerShellView_ = std::move(nextShellView);
    explorerDesktopFolder_ = std::move(nextDesktopFolder);
    InitializeListViewQueryAccess();
    UpdateViewMetrics();
    for (PositionOverride& value : positionOverrides_) {
        const auto item = std::find_if(
            snapshot_.items.begin(), snapshot_.items.end(),
            [&](const DesktopViewItem& candidate) {
                return IdentitiesEqual(
                    candidate.path, value.identity);
            });
        if (item == snapshot_.items.end()) {
            continue;
        }
        value.nativeScreenPoint = item->screenPoint;
        value.screenPoint = value.viewPoint;
        if (snapshot_.listViewWindow == nullptr ||
            IsWindow(snapshot_.listViewWindow) == FALSE ||
            ClientToScreen(
                snapshot_.listViewWindow,
                &value.screenPoint) == FALSE) {
            value.screenPoint = value.nativeScreenPoint;
        }
        SetItemScreenPoint(value.identity, value.screenPoint);
    }
    RebuildVisibleItems();
    for (const DesktopViewItem& item : visibleItems_) {
        iconCache_.PreloadShellIcon(
            item.path,
            item.systemImageIndex,
            item.overlayIndex,
            iconSize_);
    }
    const int width =
        snapshot_.screenRect.right - snapshot_.screenRect.left;
    const int height =
        snapshot_.screenRect.bottom - snapshot_.screenRect.top;
    POINT parentOrigin{
        snapshot_.screenRect.left,
        snapshot_.screenRect.top};
    if (ScreenToClient(
            snapshot_.desktopHost,
            &parentOrigin) == FALSE) {
        errorMessage =
            L"无法刷新 Explorer 图标区域的父窗口坐标。";
        return false;
    }
    SetWindowPos(
        hwnd_, nullptr, parentOrigin.x,
        parentOrigin.y, width, height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    d2d_.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (refreshWallpaper && !wallpaper_.Refresh(
            hwnd_, d2d_.Target(), width, height, false)) {
        errorMessage =
            L"无法刷新与当前桌面一致的无模糊壁纸快照。";
        return false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    MaintainDesktopLayer();
    return true;
}

bool DesktopSurfaceWindow::IsDesktopHosted() const noexcept {
    return hwnd_ != nullptr &&
        snapshot_.desktopHost != nullptr &&
        IsWindow(snapshot_.desktopHost) != FALSE &&
        (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_POPUP) != 0 &&
        (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CHILD) == 0 &&
        GetAncestor(hwnd_, GA_PARENT) == snapshot_.desktopHost &&
        GetAncestor(hwnd_, GA_ROOT) == snapshot_.desktopHost;
}

LRESULT CALLBACK DesktopSurfaceWindow::WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    auto* window = reinterpret_cast<DesktopSurfaceWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create =
            reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<DesktopSurfaceWindow*>(
            create->lpCreateParams);
        SetWindowLongPtrW(
            hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    }
    return window == nullptr
        ? DefWindowProcW(hwnd, message, wParam, lParam)
        : window->HandleMessage(message, wParam, lParam);
}

LRESULT CALLBACK DesktopSurfaceWindow::RenameEditProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR referenceData) {
    auto* window = reinterpret_cast<DesktopSurfaceWindow*>(referenceData);
    if (message == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    if (message == WM_KEYDOWN && window != nullptr) {
        if (wParam == VK_RETURN) {
            PostMessageW(window->Window(), kFinishRenameMessage, 0, 0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            PostMessageW(window->Window(), kCancelRenameMessage, 0, 0);
            return 0;
        }
    }
    if (message == WM_KILLFOCUS && window != nullptr &&
        !window->renameFinalizing_) {
        PostMessageW(window->Window(), kFinishRenameMessage, 0, 0);
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, RenameEditProc, subclassId);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT DesktopSurfaceWindow::HandleMessage(
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    if (message == WM_INITMENUPOPUP || message == WM_DRAWITEM ||
        message == WM_MEASUREITEM || message == WM_MENUCHAR) {
        LRESULT menuResult = 0;
        if (launcher_.ForwardContextMenuMessage(
                message, wParam, lParam, menuResult)) {
            return menuResult;
        }
    }
    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Render();
            return 0;
        case kIconReadyMessage:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case kFinishRenameMessage:
            FinishRename(true);
            return 0;
        case kCancelRenameMessage:
            FinishRename(false);
            return 0;
        case kBeginRenameMessage:
            if (renameEdit_ == nullptr &&
                desktopKeyboardSelectionArmed_ &&
                selectedIdentities_.size() == 1) {
                BeginRename(*selectedIdentities_.begin());
            }
            return 0;
        case WM_TIMER:
            if (wParam == kRenameTimerId) {
                KillTimer(hwnd_, kRenameTimerId);
                const std::wstring identity =
                    pendingRenameIdentity_;
                renameClickCandidate_ = false;
                pendingRenameIdentity_.clear();
                if (!identity.empty() &&
                    selectedIdentities_.size() == 1 &&
                    IsSelected(identity)) {
                    BeginRename(identity);
                }
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_F2 &&
                renameEdit_ == nullptr &&
                selectedIdentities_.size() == 1) {
                BeginRename(*selectedIdentities_.begin());
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            const POINT current{
                GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pointerGesture_ != PointerGesture::None &&
                (wParam & MK_LBUTTON) == 0) {
                CancelPointerCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (
                pointerGesture_ != PointerGesture::None) {
                const POINT dragSourceClientPoint = pointerStart_;
                const std::vector<std::wstring> dragPaths =
                    ContinuePointerGesture(current);
                hoverIndex_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (!dragPaths.empty()) {
                    CancelPendingRename();
                    const std::vector<ShellItemReference>
                        dragItems =
                            SelectedShellItemsInVisibleOrder();
                    if (GetCapture() == hwnd_) {
                        ReleaseCapture();
                    }
                    ++shellDragStartCount_;
                    if (!suppressShellDragForSmoke_) {
                        SHDRAGIMAGE dragImage{};
                        const bool hasDragImage =
                            BuildShellDragImage(
                                dragSourceClientPoint,
                                dragImage);
                        if (BeginInternalDragSession(
                                dragSourceClientPoint)) {
                            StartShellDrag(
                                hwnd_,
                                dragItems,
                                hasDragImage ? &dragImage : nullptr);
                            EndInternalDragSession();
                        }
                        if (dragImage.hbmpDragImage != nullptr) {
                            DeleteObject(dragImage.hbmpDragImage);
                        }
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
                return 0;
            }
            const int hover = HitTest(current);
            if (hover != hoverIndex_) {
                hoverIndex_ = hover;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{
                sizeof(track), TME_LEAVE, hwnd_, HOVER_DEFAULT};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverIndex_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            if (renameEdit_ != nullptr) {
                FinishRename(true);
                if (renameEdit_ != nullptr) {
                    return 0;
                }
            }
            CancelPendingRename();
            const POINT clientPoint{
                GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const bool controlPressed =
                (wParam & MK_CONTROL) != 0 ||
                (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const int index = HitTest(clientPoint);
            if (index >= 0 && !controlPressed) {
                const DesktopViewItem& item =
                    visibleItems_[static_cast<size_t>(index)];
                const RECT label = LabelRect(item);
                renameClickCandidate_ =
                    selectedIdentities_.size() == 1 &&
                    IsSelected(item.path) &&
                    PtInRect(&label, clientPoint) != FALSE;
                if (renameClickCandidate_) {
                    pendingRenameIdentity_ = item.path;
                }
            }
            BeginPointerGesture(clientPoint, controlPressed);
            SynchronizeExplorerSelection();
            desktopKeyboardSelectionArmed_ =
                !selectedIdentities_.empty();
            hoverIndex_ = -1;
            if (pointerGesture_ != PointerGesture::None) {
                SetCapture(hwnd_);
                if (GetCapture() != hwnd_) {
                    CompletePointerGesture();
                }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const bool scheduleRename =
                renameClickCandidate_ &&
                pointerGesture_ == PointerGesture::ItemPressed &&
                !pendingRenameIdentity_.empty();
            CompletePointerGesture();
            SynchronizeExplorerSelection();
            if (GetCapture() == hwnd_) {
                ReleaseCapture();
            }
            if (scheduleRename &&
                selectedIdentities_.size() == 1 &&
                IsSelected(pendingRenameIdentity_)) {
                if (SetTimer(
                        hwnd_,
                        kRenameTimerId,
                        GetDoubleClickTime(),
                        nullptr) == 0) {
                    CancelPendingRename();
                }
            } else {
                CancelPendingRename();
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            ResetPointerGesture();
            SynchronizeExplorerSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CANCELMODE:
            CancelPendingRename();
            CancelPointerCapture();
            SynchronizeExplorerSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDBLCLK: {
            CancelPendingRename();
            desktopKeyboardSelectionArmed_ = false;
            const int index = HitTest(
                POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (index >= 0) {
                SelectOnly(
                    visibleItems_[static_cast<size_t>(index)].path);
                SynchronizeExplorerSelection();
                CancelPointerCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);
                launcher_.OpenPath(
                    visibleItems_[static_cast<size_t>(index)].path);
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            CancelPendingRename();
            if (renameEdit_ != nullptr) {
                FinishRename(true);
                if (renameEdit_ != nullptr) {
                    return 0;
                }
            }
            CancelPointerCapture();
            POINT screenPoint{
                GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (screenPoint.x == -1 && screenPoint.y == -1) {
                GetCursorPos(&screenPoint);
            }
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd_, &clientPoint);
            const int index = HitTest(clientPoint);
            if (index >= 0) {
                const std::wstring& identity =
                    visibleItems_[static_cast<size_t>(index)].path;
                if (!IsSelected(identity)) {
                    SelectOnly(identity);
                }
                SynchronizeExplorerSelection();
                desktopKeyboardSelectionArmed_ = true;
                const std::vector<ShellItemReference>
                    selectedItems =
                        SelectedShellItemsInVisibleOrder();
                bool canRename = false;
                if (selectedItems.size() == 1) {
                    CanRenameDesktopShellItem(
                        selectedItems.front(), canRename);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                const ShellContextMenuResult menuResult =
                    launcher_.ShowDesktopContextMenu(
                    hwnd_,
                    selectedItems,
                    screenPoint,
                    canRename);
                if (menuResult ==
                        ShellContextMenuResult::RenameRequested &&
                    selectedItems.size() == 1) {
                    BeginRename(selectedItems.front().path);
                }
            } else {
                desktopKeyboardSelectionArmed_ = false;
                selectedIdentities_.clear();
                SynchronizeExplorerSelection();
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (snapshot_.listViewWindow != nullptr &&
                    IsWindow(snapshot_.listViewWindow) != FALSE) {
                    SendMessageW(
                        snapshot_.listViewWindow,
                        WM_CONTEXTMENU,
                        reinterpret_cast<WPARAM>(
                            snapshot_.listViewWindow),
                        MAKELPARAM(screenPoint.x, screenPoint.y));
                }
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {
            std::wstring ignored;
            Refresh(ignored);
            return 0;
        }
        case WM_DESTROY:
            RemoveKeyboardHook();
            CancelPendingRename();
            if (renameEdit_ != nullptr) {
                RemoveWindowSubclass(
                    renameEdit_, RenameEditProc,
                    kRenameSubclassId);
                renameEdit_ = nullptr;
            }
            if (renameFont_ != nullptr) {
                DeleteObject(renameFont_);
                renameFont_ = nullptr;
            }
            renameIdentity_.clear();
            renameOriginalDisplayName_.clear();
            iconCache_.SetInvalidateCallback(nullptr);
            if (dropTargetRegistered_) {
                RevokeDragDrop(hwnd_);
            }
            dropTargetRegistered_ = false;
            desktopDropTarget_.Reset();
            ResetPointerGesture();
            hwnd_ = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void DesktopSurfaceWindow::Render() {
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);
    ID2D1HwndRenderTarget* target = d2d_.Target();
    if (target == nullptr) {
        EndPaint(hwnd_, &paint);
        return;
    }
    d2d_.BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    RECT client{};
    GetClientRect(hwnd_, &client);
    wallpaper_.Draw(
        target,
        D2D1::RectF(
            0.0f, 0.0f,
            static_cast<FLOAT>(client.right),
            static_cast<FLOAT>(client.bottom)),
        kWallpaperDrawMode);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selectionBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selectionBorder;
    target->CreateSolidColorBrush(
        D2D1::ColorF(0xFFFFFF, 1.0f), textBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(0x000000, 0.90f), shadowBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(0x2D8CFF, 0.22f),
        selectionBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(0xB9D7FF, 0.92f),
        selectionBorder.GetAddressOf());

    const DesktopLabelStyle labelStyle = ReadDesktopLabelStyle();
    Microsoft::WRL::ComPtr<IDWriteTextFormat> labelFormat;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsisSign;
    d2d_.WriteFactory()->CreateTextFormat(
        labelStyle.faceName.c_str(),
        nullptr,
        labelStyle.weight,
        labelStyle.style,
        DWRITE_FONT_STRETCH_NORMAL,
        labelStyle.size,
        L"zh-cn",
        labelFormat.GetAddressOf());
    if (labelFormat != nullptr) {
        labelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        labelFormat->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        labelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        if (SUCCEEDED(d2d_.WriteFactory()->CreateEllipsisTrimmingSign(
                labelFormat.Get(), ellipsisSign.GetAddressOf()))) {
            const DWRITE_TRIMMING trimming{
                DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            labelFormat->SetTrimming(&trimming, ellipsisSign.Get());
        }
    }

    if (pointerGesture_ == PointerGesture::MarqueeActive &&
        !IsRectEmpty(&marqueeRect_) &&
        selectionBrush != nullptr &&
        selectionBorder != nullptr) {
        const D2D1_RECT_F marquee = D2D1::RectF(
            static_cast<FLOAT>(marqueeRect_.left),
            static_cast<FLOAT>(marqueeRect_.top),
            static_cast<FLOAT>(marqueeRect_.right),
            static_cast<FLOAT>(marqueeRect_.bottom));
        target->FillRectangle(marquee, selectionBrush.Get());
        target->DrawRectangle(
            marquee, selectionBorder.Get(), 1.0f);
    }

    for (size_t index = 0; index < visibleItems_.size(); ++index) {
        const DesktopViewItem& item = visibleItems_[index];
        const RECT cell = CellRect(item);
        const RECT interaction = InteractionRect(index);
        if ((IsSelected(item.path) ||
             (pointerGesture_ == PointerGesture::None &&
              static_cast<int>(index) == hoverIndex_)) &&
            selectionBrush != nullptr &&
            selectionBorder != nullptr) {
            const D2D1_ROUNDED_RECT selection = D2D1::RoundedRect(
                D2D1::RectF(
                    static_cast<FLOAT>(interaction.left),
                    static_cast<FLOAT>(interaction.top),
                    static_cast<FLOAT>(interaction.right),
                    static_cast<FLOAT>(interaction.bottom)),
                1.0f,
                1.0f);
            target->FillRoundedRectangle(
                selection, selectionBrush.Get());
            target->DrawRoundedRectangle(
                selection, selectionBorder.Get(), 1.0f);
        }
        const FLOAT iconLeft = static_cast<FLOAT>(
            cell.left + (cellWidth_ - iconSize_) / 2);
        const FLOAT iconTop = static_cast<FLOAT>(cell.top);
        const D2D1_RECT_F iconRect = D2D1::RectF(
            iconLeft,
            iconTop,
            iconLeft + static_cast<FLOAT>(iconSize_),
            iconTop + static_cast<FLOAT>(iconSize_));
        ID2D1Bitmap* bitmap = iconCache_.GetIcon(
            target,
            item.path,
            item.displayName,
            IconPlaceholderKind::File,
            nullptr,
            item.systemImageIndex,
            item.overlayIndex,
            iconSize_);
        if (bitmap != nullptr) {
            const FLOAT bitmapHeight = item.overlayIndex > 0
                ? static_cast<FLOAT>(
                    iconSize_ + std::max(1, iconSize_ / 14))
                : static_cast<FLOAT>(iconSize_);
            target->DrawBitmap(
                bitmap,
                D2D1::RectF(
                    iconRect.left,
                    iconRect.top,
                    iconRect.right,
                    iconRect.top + bitmapHeight),
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        if (labelFormat == nullptr ||
            textBrush == nullptr ||
            shadowBrush == nullptr) {
            continue;
        }
        if (renameEdit_ != nullptr &&
            IdentitiesEqual(item.path, renameIdentity_)) {
            continue;
        }
        const D2D1_RECT_F labelRect = D2D1::RectF(
            static_cast<FLOAT>(cell.left + 4),
            iconRect.bottom + 9.0f,
            static_cast<FLOAT>(cell.right - 4),
            static_cast<FLOAT>(cell.bottom));
        target->PushAxisAlignedClip(
            labelRect, D2D1_ANTIALIAS_MODE_ALIASED);
        constexpr D2D1_POINT_2F offsets[] = {
            {-1.0f, 0.0f}, {1.0f, 0.0f},
            {0.0f, -1.0f}, {0.0f, 1.0f}};
        for (const D2D1_POINT_2F offset : offsets) {
            const D2D1_RECT_F shadowRect = D2D1::RectF(
                labelRect.left + offset.x,
                labelRect.top + offset.y,
                labelRect.right + offset.x,
                labelRect.bottom + offset.y);
            target->DrawTextW(
                item.displayName.c_str(),
                static_cast<UINT32>(item.displayName.size()),
                labelFormat.Get(),
                shadowRect,
                shadowBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        target->DrawTextW(
            item.displayName.c_str(),
            static_cast<UINT32>(item.displayName.size()),
            labelFormat.Get(),
            labelRect,
            textBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        target->PopAxisAlignedClip();
    }
    const HRESULT result = d2d_.EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
        d2d_.RecreateTarget(hwnd_);
        ConfigurePixelRenderTarget();
        const int width =
            snapshot_.screenRect.right - snapshot_.screenRect.left;
        const int height =
            snapshot_.screenRect.bottom - snapshot_.screenRect.top;
        wallpaper_.Refresh(
            hwnd_, d2d_.Target(), width, height, false);
    }
    EndPaint(hwnd_, &paint);
}

void DesktopSurfaceWindow::UpdateViewMetrics() {
    const LRESULT spacing = SendMessageW(
        snapshot_.listViewWindow, LVM_GETITEMSPACING, FALSE, 0);
    if (spacing != 0) {
        cellWidth_ = std::max(
            48, static_cast<int>(LOWORD(spacing)));
        cellHeight_ = std::max(
            48, static_cast<int>(HIWORD(spacing)));
    }
    UINT viewDpi = 96;
    if (snapshot_.listViewWindow != nullptr) {
        const UINT reportedDpi = GetDpiForWindow(
            snapshot_.listViewWindow);
        if (reportedDpi != 0) {
            viewDpi = reportedDpi;
        }
    }
    iconSize_ = std::clamp(
        MulDiv(snapshot_.viewIconSize,
               static_cast<int>(viewDpi), 96),
        16, 512);
}

void DesktopSurfaceWindow::ConfigurePixelRenderTarget() {
    if (d2d_.Target() != nullptr) {
        d2d_.Target()->SetDpi(96.0f, 96.0f);
    }
}

int DesktopSurfaceWindow::HitTest(POINT clientPoint) const {
    int nativeIndex = -1;
    if (TryNativeHitTest(clientPoint, nativeIndex)) {
        return nativeIndex;
    }
    for (size_t index = 0; index < visibleItems_.size(); ++index) {
        if (IsInFallbackHitRegion(
                visibleItems_[index], clientPoint)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

RECT DesktopSurfaceWindow::CellRect(
    const DesktopViewItem& item) const {
    const int left =
        item.screenPoint.x - snapshot_.screenRect.left -
        (cellWidth_ - iconSize_) / 2;
    const int top =
        item.screenPoint.y - snapshot_.screenRect.top;
    return RECT{
        left, top, left + cellWidth_, top + cellHeight_};
}

RECT DesktopSurfaceWindow::LabelRect(
    const DesktopViewItem& item) const {
    const RECT cell = CellRect(item);
    return RECT{
        cell.left + 4,
        cell.top + iconSize_ + 9,
        cell.right - 4,
        cell.bottom};
}

RECT DesktopSurfaceWindow::FallbackInteractionRect(
    const DesktopViewItem& item) const {
    const RECT cell = CellRect(item);
    const LONG iconLeft = cell.left +
        (cellWidth_ - iconSize_) / 2;
    RECT result{
        iconLeft,
        cell.top,
        iconLeft + iconSize_,
        cell.top + iconSize_};
    const RECT label = LabelRect(item);
    if (!IsRectEmpty(&label)) {
        UnionRect(&result, &result, &label);
    }
    return result;
}

bool DesktopSurfaceWindow::IsInFallbackHitRegion(
    const DesktopViewItem& item,
    POINT clientPoint) const {
    const RECT cell = CellRect(item);
    const LONG iconLeft = cell.left +
        (cellWidth_ - iconSize_) / 2;
    const RECT icon{
        iconLeft,
        cell.top,
        iconLeft + iconSize_,
        cell.top + iconSize_};
    const RECT label{
        cell.left + 4,
        cell.top + iconSize_ + 9,
        cell.right - 4,
        cell.bottom};
    return PtInRect(&icon, clientPoint) != FALSE ||
        PtInRect(&label, clientPoint) != FALSE;
}

RECT DesktopSurfaceWindow::InteractionRect(
    size_t visibleIndex) const {
    if (visibleIndex < visibleInteractionRects_.size()) {
        return visibleInteractionRects_[visibleIndex];
    }
    return visibleIndex < visibleItems_.size()
        ? FallbackInteractionRect(visibleItems_[visibleIndex])
        : RECT{};
}

bool DesktopSurfaceWindow::TryNativeHitTest(
    POINT clientPoint,
    int& visibleIndex) const {
    visibleIndex = -1;
    if (!listViewQueryReady_ || hwnd_ == nullptr ||
        snapshot_.listViewWindow == nullptr ||
        IsWindow(hwnd_) == FALSE ||
        IsWindow(snapshot_.listViewWindow) == FALSE) {
        return false;
    }
    POINT listViewPoint = clientPoint;
    SetLastError(ERROR_SUCCESS);
    const int mapped = MapWindowPoints(
        hwnd_, snapshot_.listViewWindow,
        &listViewPoint, 1);
    if (mapped == 0 && GetLastError() != ERROR_SUCCESS) {
        return false;
    }
    const auto queryNativeHit =
        [&](POINT point, LVHITTESTINFO& hit) {
            hit = {};
            hit.pt = point;
            hit.iItem = -1;
            LRESULT messageResult = -1;
            return SendListViewQuery(
                LVM_HITTEST, 0, &hit, sizeof(hit),
                messageResult);
        };
    constexpr UINT kItemFlags =
        LVHT_ONITEMICON | LVHT_ONITEMLABEL |
        LVHT_ONITEMSTATEICON;
    for (size_t index = 0; index < visibleItems_.size(); ++index) {
        const DesktopViewItem& item = visibleItems_[index];
        const auto position = std::find_if(
            positionOverrides_.begin(), positionOverrides_.end(),
            [&](const PositionOverride& value) {
                return IdentitiesEqual(value.identity, item.path);
            });
        if (position == positionOverrides_.end()) {
            continue;
        }
        RECT translatedRect{};
        if (!TryNativeInteractionRect(item, translatedRect) ||
            PtInRect(&translatedRect, clientPoint) == FALSE) {
            continue;
        }
        POINT nativePoint{
            listViewPoint.x -
                (position->screenPoint.x -
                 position->nativeScreenPoint.x),
            listViewPoint.y -
                (position->screenPoint.y -
                 position->nativeScreenPoint.y)};
        LVHITTESTINFO translatedHit{};
        if (!queryNativeHit(nativePoint, translatedHit)) {
            return false;
        }
        if (translatedHit.iItem == item.viewIndex &&
            (translatedHit.flags & kItemFlags) != 0) {
            visibleIndex = static_cast<int>(index);
        }
        return true;
    }

    LVHITTESTINFO hit{};
    if (!queryNativeHit(listViewPoint, hit)) {
        return false;
    }
    if (hit.iItem < 0 ||
        (hit.flags & kItemFlags) == 0) {
        return true;
    }
    const auto item = std::find_if(
        visibleItems_.begin(), visibleItems_.end(),
        [&](const DesktopViewItem& candidate) {
            return candidate.viewIndex == hit.iItem;
        });
    const bool hasDisplayOverride =
        item != visibleItems_.end() &&
        std::any_of(
            positionOverrides_.begin(), positionOverrides_.end(),
            [&](const PositionOverride& value) {
                return IdentitiesEqual(value.identity, item->path);
            });
    if (item != visibleItems_.end() && !hasDisplayOverride) {
        visibleIndex = static_cast<int>(
            std::distance(visibleItems_.begin(), item));
    }
    return true;
}

bool DesktopSurfaceWindow::TryNativeInteractionRect(
    const DesktopViewItem& item,
    RECT& interactionRect) const {
    interactionRect = {};
    if (!listViewQueryReady_ || item.viewIndex < 0 ||
        hwnd_ == nullptr || snapshot_.listViewWindow == nullptr ||
        IsWindow(hwnd_) == FALSE ||
        IsWindow(snapshot_.listViewWindow) == FALSE) {
        return false;
    }
    RECT nativeRect{};
    nativeRect.left = LVIR_SELECTBOUNDS;
    LRESULT messageResult = FALSE;
    if (!SendListViewQuery(
            LVM_GETITEMRECT,
            static_cast<WPARAM>(item.viewIndex),
            &nativeRect,
            sizeof(nativeRect),
            messageResult) || messageResult == FALSE) {
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const int mapped = MapWindowPoints(
        snapshot_.listViewWindow,
        hwnd_,
        reinterpret_cast<POINT*>(&nativeRect),
        2);
    if (mapped == 0 && GetLastError() != ERROR_SUCCESS) {
        return false;
    }
    if (IsRectEmpty(&nativeRect)) {
        return false;
    }
    const auto position = std::find_if(
        positionOverrides_.begin(), positionOverrides_.end(),
        [&](const PositionOverride& value) {
            return IdentitiesEqual(value.identity, item.path);
        });
    if (position != positionOverrides_.end()) {
        const LONG offsetX =
            position->screenPoint.x - position->nativeScreenPoint.x;
        const LONG offsetY =
            position->screenPoint.y - position->nativeScreenPoint.y;
        OffsetRect(&nativeRect, offsetX, offsetY);
    }
    interactionRect = nativeRect;
    return true;
}

bool DesktopSurfaceWindow::SendListViewQuery(
    UINT message,
    WPARAM wParam,
    void* localBuffer,
    size_t bufferSize,
    LRESULT& messageResult) const {
    messageResult = 0;
    if (!listViewQueryReady_ || localBuffer == nullptr ||
        bufferSize == 0 || snapshot_.listViewWindow == nullptr ||
        IsWindow(snapshot_.listViewWindow) == FALSE) {
        return false;
    }
    DWORD currentProcessId = 0;
    GetWindowThreadProcessId(
        snapshot_.listViewWindow, &currentProcessId);
    if (currentProcessId == 0 ||
        currentProcessId != listViewProcessId_) {
        return false;
    }
    void* messageBuffer = localBuffer;
    if (currentProcessId != GetCurrentProcessId()) {
        if (listViewProcess_ == nullptr ||
            listViewQueryBuffer_ == nullptr) {
            return false;
        }
        SIZE_T written = 0;
        if (WriteProcessMemory(
                listViewProcess_,
                listViewQueryBuffer_,
                localBuffer,
                bufferSize,
                &written) == FALSE ||
            written != bufferSize) {
            return false;
        }
        messageBuffer = listViewQueryBuffer_;
    }
    DWORD_PTR rawResult = 0;
    if (SendMessageTimeoutW(
            snapshot_.listViewWindow,
            message,
            wParam,
            reinterpret_cast<LPARAM>(messageBuffer),
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            kListViewQueryTimeoutMilliseconds,
            &rawResult) == 0) {
        return false;
    }
    if (currentProcessId != GetCurrentProcessId()) {
        SIZE_T read = 0;
        if (ReadProcessMemory(
                listViewProcess_,
                listViewQueryBuffer_,
                localBuffer,
                bufferSize,
                &read) == FALSE ||
            read != bufferSize) {
            return false;
        }
    }
    messageResult = static_cast<LRESULT>(rawResult);
    return true;
}

bool DesktopSurfaceWindow::InitializeListViewQueryAccess() noexcept {
    ReleaseListViewQueryAccess();
    if (snapshot_.listViewWindow == nullptr ||
        IsWindow(snapshot_.listViewWindow) == FALSE) {
        return false;
    }
    GetWindowThreadProcessId(
        snapshot_.listViewWindow,
        &listViewProcessId_);
    if (listViewProcessId_ == 0) {
        return false;
    }
    if (listViewProcessId_ == GetCurrentProcessId()) {
        listViewQueryReady_ = true;
        return true;
    }
    listViewProcess_ = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_READ |
            PROCESS_VM_WRITE,
        FALSE,
        listViewProcessId_);
    if (listViewProcess_ == nullptr) {
        listViewProcessId_ = 0;
        return false;
    }
    constexpr SIZE_T kQueryBufferSize =
        sizeof(LVHITTESTINFO) > sizeof(RECT)
            ? sizeof(LVHITTESTINFO)
            : sizeof(RECT);
    listViewQueryBuffer_ = VirtualAllocEx(
        listViewProcess_,
        nullptr,
        kQueryBufferSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (listViewQueryBuffer_ == nullptr) {
        CloseHandle(listViewProcess_);
        listViewProcess_ = nullptr;
        listViewProcessId_ = 0;
        return false;
    }
    listViewQueryReady_ = true;
    return true;
}

void DesktopSurfaceWindow::ReleaseListViewQueryAccess() noexcept {
    listViewQueryReady_ = false;
    if (listViewProcess_ != nullptr &&
        listViewQueryBuffer_ != nullptr) {
        VirtualFreeEx(
            listViewProcess_,
            listViewQueryBuffer_,
            0,
            MEM_RELEASE);
    }
    listViewQueryBuffer_ = nullptr;
    if (listViewProcess_ != nullptr) {
        CloseHandle(listViewProcess_);
    }
    listViewProcess_ = nullptr;
    listViewProcessId_ = 0;
}

void DesktopSurfaceWindow::RebuildInteractionRects() {
    visibleInteractionRects_.clear();
    visibleInteractionRects_.reserve(visibleItems_.size());
    for (const DesktopViewItem& item : visibleItems_) {
        RECT interaction = FallbackInteractionRect(item);
        RECT nativeRect{};
        if (TryNativeInteractionRect(item, nativeRect)) {
            interaction = nativeRect;
        }
        visibleInteractionRects_.push_back(interaction);
    }
}

RECT DesktopSurfaceWindow::ClientBounds() const noexcept {
    RECT bounds{};
    if (hwnd_ != nullptr &&
        GetClientRect(hwnd_, &bounds) != FALSE) {
        return bounds;
    }
    bounds.right = (std::max<LONG>)(
        0, snapshot_.screenRect.right - snapshot_.screenRect.left);
    bounds.bottom = (std::max<LONG>)(
        0, snapshot_.screenRect.bottom - snapshot_.screenRect.top);
    return bounds;
}

RECT DesktopSurfaceWindow::NormalizeMarqueeRect(
    POINT anchor,
    POINT current,
    const RECT& bounds) noexcept {
    if (bounds.right <= bounds.left ||
        bounds.bottom <= bounds.top) {
        return RECT{};
    }
    const LONG minimumX = (std::min)(anchor.x, current.x);
    const LONG minimumY = (std::min)(anchor.y, current.y);
    const LONG maximumX = (std::max)(anchor.x, current.x);
    const LONG maximumY = (std::max)(anchor.y, current.y);
    return RECT{
        std::clamp(minimumX, bounds.left, bounds.right),
        std::clamp(minimumY, bounds.top, bounds.bottom),
        std::clamp(
            maximumX <
                    (std::numeric_limits<LONG>::max)()
                ? maximumX + 1
                : maximumX,
            bounds.left,
            bounds.right),
        std::clamp(
            maximumY <
                    (std::numeric_limits<LONG>::max)()
                ? maximumY + 1
                : maximumY,
            bounds.top,
            bounds.bottom)};
}

bool DesktopSurfaceWindow::HasExceededDragThreshold(
    POINT anchor,
    POINT current) noexcept {
    return std::abs(current.x - anchor.x) >=
            (std::max)(1, GetSystemMetrics(SM_CXDRAG)) ||
        std::abs(current.y - anchor.y) >=
            (std::max)(1, GetSystemMetrics(SM_CYDRAG));
}

bool DesktopSurfaceWindow::IsSelected(
    const std::wstring& identity) const {
    return selectedIdentities_.find(identity) !=
        selectedIdentities_.end();
}

void DesktopSurfaceWindow::AddSelected(
    const std::wstring& identity) {
    if (!identity.empty()) {
        selectedIdentities_.insert(identity);
    }
}

void DesktopSurfaceWindow::RemoveSelected(
    const std::wstring& identity) {
    selectedIdentities_.erase(identity);
}

void DesktopSurfaceWindow::SelectOnly(
    const std::wstring& identity) {
    selectedIdentities_.clear();
    AddSelected(identity);
}

bool DesktopSurfaceWindow::ResolveExplorerViewItem(
    const DesktopViewItem& item,
    PIDLIST_RELATIVE& currentPidl) const {
    currentPidl = nullptr;
    if (explorerFolderView_ == nullptr ||
        explorerDesktopFolder_ == nullptr || item.viewIndex < 0) {
        return false;
    }
    const HRESULT result = explorerFolderView_->Item(
        item.viewIndex, &currentPidl);
    STRRET parsingName{};
    HRESULT identityResult = result;
    if (SUCCEEDED(identityResult) && currentPidl != nullptr) {
        identityResult = explorerDesktopFolder_->GetDisplayNameOf(
            currentPidl, SHGDN_FORPARSING, &parsingName);
    }
    wchar_t buffer[32768]{};
    if (SUCCEEDED(identityResult)) {
        identityResult = StrRetToBufW(
            &parsingName, currentPidl, buffer, ARRAYSIZE(buffer));
    }
    const bool matches = SUCCEEDED(identityResult) &&
        CompareStringOrdinal(
            buffer, -1, item.path.c_str(), -1, TRUE) == CSTR_EQUAL;
    if (!matches) {
        CoTaskMemFree(currentPidl);
        currentPidl = nullptr;
    }
    return matches;
}

bool DesktopSurfaceWindow::ActivateExplorerDesktopView() const noexcept {
    const HWND listView = snapshot_.listViewWindow;
    if (listView == nullptr || IsWindow(listView) == FALSE) {
        return false;
    }

    DWORD explorerProcessId = 0;
    const DWORD explorerThreadId = GetWindowThreadProcessId(
        listView, &explorerProcessId);
    const DWORD currentThreadId = GetCurrentThreadId();
    if (explorerThreadId == 0 || explorerProcessId == 0 ||
        currentThreadId == 0) {
        return false;
    }

    const bool needsAttachment = explorerThreadId != currentThreadId;
    if (needsAttachment &&
        AttachThreadInput(currentThreadId, explorerThreadId, TRUE) == FALSE) {
        return false;
    }

    HWND desktopRoot = GetAncestor(listView, GA_ROOT);
    if (desktopRoot == nullptr || IsWindow(desktopRoot) == FALSE) {
        desktopRoot = snapshot_.desktopHost;
    }
    if (desktopRoot != nullptr && IsWindow(desktopRoot) != FALSE) {
        SetForegroundWindow(desktopRoot);
        SetActiveWindow(desktopRoot);
    }
    SetFocus(listView);

    GUITHREADINFO information{};
    information.cbSize = sizeof(information);
    const bool focused =
        GetGUIThreadInfo(explorerThreadId, &information) != FALSE &&
        information.hwndFocus == listView;

    if (needsAttachment) {
        AttachThreadInput(currentThreadId, explorerThreadId, FALSE);
    }
    return focused;
}

bool DesktopSurfaceWindow::SynchronizeExplorerSelection() {
    lastExplorerSelectionSyncStage_ = 1;
    lastExplorerSelectionSyncResult_ = E_NOINTERFACE;
    if (explorerFolderView_ == nullptr || explorerShellView_ == nullptr ||
        explorerDesktopFolder_ == nullptr) {
        return false;
    }
    std::vector<const DesktopViewItem*> selectedItems;
    std::vector<PIDLIST_RELATIVE> currentPidls;
    selectedItems.reserve(selectedIdentities_.size());
    currentPidls.reserve(selectedIdentities_.size());
    const auto releaseCurrentPidls = [&]() {
        for (PIDLIST_RELATIVE pidl : currentPidls) {
            CoTaskMemFree(pidl);
        }
        currentPidls.clear();
    };
    for (const DesktopViewItem& item : visibleItems_) {
        if (!IsSelected(item.path)) {
            continue;
        }
        PIDLIST_RELATIVE currentPidl = nullptr;
        if (!ResolveExplorerViewItem(item, currentPidl)) {
            lastExplorerSelectionSyncStage_ = 2;
            lastExplorerSelectionSyncResult_ =
                HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            releaseCurrentPidls();
            return false;
        }
        selectedItems.push_back(&item);
        currentPidls.push_back(currentPidl);
    }
    if (selectedItems.size() != selectedIdentities_.size()) {
        lastExplorerSelectionSyncStage_ = 2;
        lastExplorerSelectionSyncResult_ =
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        releaseCurrentPidls();
        return false;
    }

    lastExplorerSelectionSyncStage_ = 3;
    if (!ActivateExplorerDesktopView()) {
        lastExplorerSelectionSyncResult_ =
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        releaseCurrentPidls();
        return false;
    }

    lastExplorerSelectionSyncStage_ = 4;
    HRESULT result = explorerShellView_->SelectItem(
        nullptr,
        SVSI_DESELECTOTHERS);
    lastExplorerSelectionSyncResult_ = result;
    for (size_t index = 0;
         SUCCEEDED(result) && index < selectedItems.size(); ++index) {
        lastExplorerSelectionSyncStage_ = 5;
        DWORD flags = SVSI_SELECT;
        if (index == 0) {
            flags |= SVSI_FOCUSED | SVSI_SELECTIONMARK;
        }
        result = explorerShellView_->SelectItem(
            currentPidls[index], flags);
        lastExplorerSelectionSyncResult_ = result;
    }
    if (SUCCEEDED(result)) {
        lastExplorerSelectionSyncStage_ = 6;
        lastExplorerSelectionSyncResult_ = S_OK;
    }
    releaseCurrentPidls();
    return SUCCEEDED(result);
}

void DesktopSurfaceWindow::PruneSelectionToVisibleItems() {
    const auto isVisible = [&](const std::wstring& identity) {
        return std::any_of(
            visibleItems_.begin(),
            visibleItems_.end(),
            [&](const DesktopViewItem& item) {
                return IdentitiesEqual(item.path, identity);
            });
    };
    const auto prune = [&](IdentitySet& identities) {
        for (auto item = identities.begin();
             item != identities.end();) {
            if (!isVisible(*item)) {
                item = identities.erase(item);
            } else {
                ++item;
            }
        }
    };
    prune(selectedIdentities_);
    prune(selectionBaseline_);
    if (!pressedIdentity_.empty() &&
        !isVisible(pressedIdentity_)) {
        ResetPointerGesture();
    }
}

std::vector<std::wstring>
DesktopSurfaceWindow::SelectedPathsInVisibleOrder() const {
    std::vector<std::wstring> result;
    result.reserve(selectedIdentities_.size());
    for (const DesktopViewItem& item : visibleItems_) {
        if (IsSelected(item.path)) {
            result.push_back(item.path);
        }
    }
    return result;
}

std::vector<ShellItemReference>
DesktopSurfaceWindow::SelectedShellItemsInVisibleOrder() const {
    std::vector<ShellItemReference> result;
    result.reserve(selectedIdentities_.size());
    for (const DesktopViewItem& item : visibleItems_) {
        if (IsSelected(item.path)) {
            result.push_back(ShellItemReference{
                item.path,
                item.shellChildPidl});
        }
    }
    return result;
}

void DesktopSurfaceWindow::BeginPointerGesture(
    POINT clientPoint,
    bool controlPressed) {
    ResetPointerGesture();
    pointerStart_ = clientPoint;
    pointerCurrent_ = clientPoint;
    controlAtPointerDown_ = controlPressed;
    selectionBaseline_ = selectedIdentities_;
    const int index = HitTest(clientPoint);
    if (index < 0) {
        pointerGesture_ = PointerGesture::MarqueePending;
        if (!controlPressed) {
            selectedIdentities_.clear();
        }
        return;
    }

    pointerGesture_ = PointerGesture::ItemPressed;
    pressedIdentity_ =
        visibleItems_[static_cast<size_t>(index)].path;
    pressedWasSelected_ = IsSelected(pressedIdentity_);
    if (controlPressed) {
        if (!pressedWasSelected_) {
            AddSelected(pressedIdentity_);
        }
    } else if (!pressedWasSelected_) {
        SelectOnly(pressedIdentity_);
    }
}

std::vector<std::wstring>
DesktopSurfaceWindow::ContinuePointerGesture(
    POINT clientPoint) {
    pointerCurrent_ = clientPoint;
    if (pointerGesture_ == PointerGesture::None) {
        return {};
    }
    if (pointerGesture_ != PointerGesture::MarqueeActive &&
        !HasExceededDragThreshold(
            pointerStart_, pointerCurrent_)) {
        return {};
    }
    if (pointerGesture_ == PointerGesture::ItemPressed) {
        std::vector<std::wstring> paths =
            SelectedPathsInVisibleOrder();
        ResetPointerGesture();
        return paths;
    }
    if (pointerGesture_ == PointerGesture::MarqueePending) {
        pointerGesture_ = PointerGesture::MarqueeActive;
    }
    if (pointerGesture_ == PointerGesture::MarqueeActive) {
        marqueeRect_ = NormalizeMarqueeRect(
            pointerStart_, pointerCurrent_, ClientBounds());
        ApplyMarqueeSelection(marqueeRect_);
    }
    return {};
}

void DesktopSurfaceWindow::CompletePointerGesture() {
    if (pointerGesture_ == PointerGesture::ItemPressed &&
        !pressedIdentity_.empty()) {
        if (controlAtPointerDown_) {
            selectedIdentities_ = selectionBaseline_;
            if (pressedWasSelected_) {
                RemoveSelected(pressedIdentity_);
            } else {
                AddSelected(pressedIdentity_);
            }
        } else {
            SelectOnly(pressedIdentity_);
        }
    }
    ResetPointerGesture();
}

void DesktopSurfaceWindow::ResetPointerGesture() noexcept {
    selectionBaseline_.clear();
    pressedIdentity_.clear();
    pointerStart_ = {};
    pointerCurrent_ = {};
    marqueeRect_ = {};
    pointerGesture_ = PointerGesture::None;
    controlAtPointerDown_ = false;
    pressedWasSelected_ = false;
}

void DesktopSurfaceWindow::CancelPointerCapture() noexcept {
    ResetPointerGesture();
    if (hwnd_ != nullptr &&
        GetCapture() == hwnd_) {
        ReleaseCapture();
    }
}

void DesktopSurfaceWindow::CancelPendingRename() noexcept {
    if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kRenameTimerId);
    }
    renameClickCandidate_ = false;
    pendingRenameIdentity_.clear();
}

LRESULT CALLBACK DesktopSurfaceWindow::KeyboardHookProc(
    int code,
    WPARAM wParam,
    LPARAM lParam) {
    DesktopSurfaceWindow* owner = keyboardHookOwner_;
    if (code == HC_ACTION && owner != nullptr && lParam != 0) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (key->vkCode == VK_F2) {
            const bool keyDown =
                wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool keyUp =
                wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
            if (keyDown &&
                (owner->swallowF2Key_ || owner->ShouldRouteDesktopF2())) {
                if (!owner->swallowF2Key_) {
                    owner->swallowF2Key_ = PostMessageW(
                        owner->hwnd_, kBeginRenameMessage, 0, 0) != FALSE;
                }
                if (owner->swallowF2Key_) {
                    return 1;
                }
            }
            if (keyUp && owner->swallowF2Key_) {
                owner->swallowF2Key_ = false;
                return 1;
            }
        }
    }
    return CallNextHookEx(
        owner != nullptr ? owner->keyboardHook_ : nullptr,
        code,
        wParam,
        lParam);
}

bool DesktopSurfaceWindow::InstallKeyboardHook() noexcept {
    if (keyboardHook_ != nullptr) {
        return true;
    }
    if (keyboardHookOwner_ != nullptr && keyboardHookOwner_ != this) {
        return false;
    }
    keyboardHookOwner_ = this;
    keyboardHook_ = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        KeyboardHookProc,
        instance_,
        0);
    if (keyboardHook_ == nullptr) {
        keyboardHookOwner_ = nullptr;
        return false;
    }
    return true;
}

void DesktopSurfaceWindow::RemoveKeyboardHook() noexcept {
    swallowF2Key_ = false;
    if (keyboardHook_ != nullptr) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (keyboardHookOwner_ == this) {
        keyboardHookOwner_ = nullptr;
    }
}

bool DesktopSurfaceWindow::ShouldRouteDesktopF2() const noexcept {
    if (hwnd_ == nullptr || IsWindowVisible(hwnd_) == FALSE ||
        renameEdit_ != nullptr || !desktopKeyboardSelectionArmed_ ||
        selectedIdentities_.size() != 1 ||
        snapshot_.listViewWindow == nullptr ||
        IsWindow(snapshot_.listViewWindow) == FALSE) {
        return false;
    }
    const auto isDesktopContextWindow = [&](HWND window) {
        if (window == nullptr) {
            return false;
        }
        if (window == hwnd_ || window == snapshot_.desktopHost ||
            window == snapshot_.shellViewWindow ||
            window == snapshot_.listViewWindow) {
            return true;
        }
        DWORD desktopProcessId = 0;
        GetWindowThreadProcessId(
            snapshot_.listViewWindow, &desktopProcessId);
        const HWND root = GetAncestor(window, GA_ROOT);
        DWORD rootProcessId = 0;
        if (root == nullptr || desktopProcessId == 0 ||
            GetWindowThreadProcessId(root, &rootProcessId) == 0 ||
            rootProcessId != desktopProcessId) {
            return false;
        }
        wchar_t className[64]{};
        return GetClassNameW(
                   root, className, ARRAYSIZE(className)) != 0 &&
            (wcscmp(className, L"Progman") == 0 ||
             wcscmp(className, L"WorkerW") == 0 ||
             wcscmp(className, L"Shell_TrayWnd") == 0);
    };
    if (!isDesktopContextWindow(GetForegroundWindow())) {
        return false;
    }
    GUITHREADINFO information{};
    information.cbSize = sizeof(information);
    return GetGUIThreadInfo(0, &information) != FALSE &&
        (information.hwndActive == nullptr ||
         isDesktopContextWindow(information.hwndActive)) &&
        (information.hwndFocus == nullptr ||
         isDesktopContextWindow(information.hwndFocus));
}

bool DesktopSurfaceWindow::BeginRename(
    const std::wstring& identity) {
    CancelPendingRename();
    if (hwnd_ == nullptr || identity.empty()) {
        return false;
    }
    if (renameEdit_ != nullptr) {
        if (IdentitiesEqual(renameIdentity_, identity)) {
            return FocusKeyboardWindow(renameEdit_);
        }
        FinishRename(false);
    }
    const auto item = std::find_if(
        visibleItems_.begin(), visibleItems_.end(),
        [&](const DesktopViewItem& value) {
            return IdentitiesEqual(value.path, identity);
        });
    if (item == visibleItems_.end()) {
        return false;
    }
    const ShellItemReference reference{
        item->path, item->shellChildPidl};
    bool canRename = false;
    if (FAILED(CanRenameDesktopShellItem(
            reference, canRename)) || !canRename) {
        return false;
    }

    RECT label = LabelRect(*item);
    const int center = (label.left + label.right) / 2;
    const LONG width = (std::max<LONG>)(
        96, label.right - label.left + 24);
    label.left = center - width / 2;
    label.right = label.left + width;
    const RECT bounds = ClientBounds();
    if (label.left < bounds.left) {
        OffsetRect(&label, bounds.left - label.left, 0);
    }
    if (label.right > bounds.right) {
        OffsetRect(&label, bounds.right - label.right, 0);
    }
    label.bottom = (std::min<LONG>)(
        bounds.bottom,
        label.top + (std::max<LONG>)(
            24, label.bottom - label.top));
    if (label.right <= label.left || label.bottom <= label.top) {
        return false;
    }

    POINT editOrigin{label.left, label.top};
    if (ClientToScreen(hwnd_, &editOrigin) == FALSE) {
        return false;
    }

    renameEdit_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_CLIENTEDGE,
        L"EDIT",
        item->displayName.c_str(),
        WS_POPUP | WS_VISIBLE | WS_TABSTOP |
            ES_CENTER | ES_AUTOHSCROLL,
        editOrigin.x,
        editOrigin.y,
        label.right - label.left,
        label.bottom - label.top,
        hwnd_,
        nullptr,
        instance_,
        nullptr);
    if (renameEdit_ == nullptr) {
        return false;
    }
    if (!SetWindowSubclass(
            renameEdit_,
            RenameEditProc,
            kRenameSubclassId,
            reinterpret_cast<DWORD_PTR>(this))) {
        DestroyWindow(renameEdit_);
        renameEdit_ = nullptr;
        return false;
    }
    LOGFONTW iconFont{};
    if (SystemParametersInfoW(
            SPI_GETICONTITLELOGFONT,
            sizeof(iconFont),
            &iconFont,
            0) != FALSE) {
        renameFont_ = CreateFontIndirectW(&iconFont);
    }
    SendMessageW(
        renameEdit_,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            renameFont_ != nullptr
                ? renameFont_
                : GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
    SendMessageW(renameEdit_, EM_SETLIMITTEXT, 255, 0);
    renameIdentity_ = item->path;
    renameOriginalDisplayName_ = item->displayName;
    SelectOnly(item->path);

    int selectionEnd = static_cast<int>(item->displayName.size());
    const DWORD attributes = GetFileAttributesW(item->path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        const size_t separator = item->path.find_last_of(L"\\/");
        const size_t extensionStart = item->path.find_last_of(L'.');
        if (extensionStart != std::wstring::npos &&
            (separator == std::wstring::npos ||
             extensionStart > separator)) {
            const std::wstring extension =
                item->path.substr(extensionStart);
            if (item->displayName.size() > extension.size() &&
                CompareStringOrdinal(
                    item->displayName.c_str() +
                        item->displayName.size() - extension.size(),
                    static_cast<int>(extension.size()),
                    extension.c_str(),
                    static_cast<int>(extension.size()),
                    TRUE) == CSTR_EQUAL) {
                selectionEnd = static_cast<int>(
                    item->displayName.size() - extension.size());
            }
        }
    }
    SendMessageW(renameEdit_, EM_SETSEL, 0, selectionEnd);
    if (!FocusKeyboardWindow(renameEdit_)) {
        RemoveWindowSubclass(
            renameEdit_, RenameEditProc, kRenameSubclassId);
        DestroyWindow(renameEdit_);
        renameEdit_ = nullptr;
        if (renameFont_ != nullptr) {
            DeleteObject(renameFont_);
            renameFont_ = nullptr;
        }
        renameIdentity_.clear();
        renameOriginalDisplayName_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void DesktopSurfaceWindow::FinishRename(bool commit) {
    if (renameEdit_ == nullptr || renameFinalizing_) {
        return;
    }
    renameFinalizing_ = true;
    const int length = GetWindowTextLengthW(renameEdit_);
    std::wstring newDisplayName(
        static_cast<size_t>((std::max)(0, length)) + 1,
        L'\0');
    if (length > 0) {
        GetWindowTextW(
            renameEdit_, newDisplayName.data(), length + 1);
        newDisplayName.resize(static_cast<size_t>(length));
    } else {
        newDisplayName.clear();
    }
    const std::wstring previousIdentity = renameIdentity_;
    const std::wstring previousDisplayName =
        renameOriginalDisplayName_;
    HWND edit = renameEdit_;
    renameEdit_ = nullptr;
    RemoveWindowSubclass(
        edit, RenameEditProc, kRenameSubclassId);
    DestroyWindow(edit);
    if (renameFont_ != nullptr) {
        DeleteObject(renameFont_);
        renameFont_ = nullptr;
    }
    renameIdentity_.clear();
    renameOriginalDisplayName_.clear();
    renameFinalizing_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!commit ||
        newDisplayName == previousDisplayName) {
        return;
    }
    if (newDisplayName.empty()) {
        MessageDialog::Show(
            instance_, hwnd_,
            L"名称不能为空。",
            L"Lattice 重命名",
            MB_OK | MB_ICONWARNING);
        BeginRename(previousIdentity);
        return;
    }

    const auto item = std::find_if(
        snapshot_.items.begin(), snapshot_.items.end(),
        [&](const DesktopViewItem& value) {
            return IdentitiesEqual(
                value.path, previousIdentity);
        });
    if (item == snapshot_.items.end()) {
        return;
    }
    const ShellItemReference reference{
        item->path, item->shellChildPidl};
    std::vector<std::wstring> otherIdentities;
    otherIdentities.reserve(snapshot_.items.size());
    for (const DesktopViewItem& candidate : snapshot_.items) {
        if (!IdentitiesEqual(
                candidate.path, previousIdentity)) {
            otherIdentities.push_back(candidate.path);
        }
    }
    DesktopShellRenameResult renamedItem;
    const HRESULT result = RenameDesktopShellItem(
        hwnd_, reference, newDisplayName, renamedItem);
    if (FAILED(result) &&
        renamedItem.disposition ==
            ShellRenameDisposition::Unchanged) {
        MessageDialog::Show(
            instance_, hwnd_,
            L"Windows 无法完成这个重命名。请检查名称是否冲突、是否包含无效字符，或该项目是否允许重命名。\n\n错误代码：" +
                std::to_wstring(static_cast<unsigned long>(result)),
            L"Lattice 重命名",
            MB_OK | MB_ICONERROR);
        if (BeginRename(previousIdentity) &&
            renameEdit_ != nullptr) {
            SetWindowTextW(renameEdit_, newDisplayName.c_str());
            SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
        }
        return;
    }

    if (renamedItem.item.path.empty() ||
        renamedItem.item.desktopChildPidl.empty()) {
        std::wstring refreshError;
        if (Refresh(refreshError)) {
            const auto isPreviousItem =
                [&](const DesktopViewItem& candidate) {
                    return std::none_of(
                        otherIdentities.begin(),
                        otherIdentities.end(),
                        [&](const std::wstring& identity) {
                            return IdentitiesEqual(
                                candidate.path, identity);
                        });
                };
            const DesktopViewItem* exactMatch = nullptr;
            size_t exactMatchCount = 0;
            size_t newIdentityCount = 0;
            for (const DesktopViewItem& candidate : snapshot_.items) {
                if (!isPreviousItem(candidate)) {
                    continue;
                }
                ++newIdentityCount;
                if (CompareStringOrdinal(
                        candidate.displayName.c_str(), -1,
                        newDisplayName.c_str(), -1,
                        TRUE) == CSTR_EQUAL) {
                    exactMatch = &candidate;
                    ++exactMatchCount;
                }
            }
            const DesktopViewItem* reconciled =
                exactMatchCount == 1 && newIdentityCount == 1
                    ? exactMatch
                    : nullptr;
            if (reconciled != nullptr) {
                renamedItem.item.path = reconciled->path;
                renamedItem.item.desktopChildPidl =
                    reconciled->shellChildPidl;
                renamedItem.displayName =
                    reconciled->displayName;
            }
        }
    }
    if (renamedItem.item.path.empty() ||
        renamedItem.item.desktopChildPidl.empty()) {
        MessageDialog::Show(
            instance_, hwnd_,
            L"Windows 已完成重命名，但 Lattice 无法确认新的 Shell 身份。桌面已刷新；请保留当前名称并重新启动 Lattice 以完成协调。\n\n错误代码：" +
                std::to_wstring(
                    static_cast<unsigned long>(result)),
            L"Lattice 重命名",
            MB_OK | MB_ICONERROR);
        return;
    }
    if (renamedItem.displayName.empty()) {
        renamedItem.displayName = newDisplayName;
    }
    const bool stateAccepted = !renameCommitHandler_ ||
        renameCommitHandler_(
            previousIdentity,
            renamedItem.item.path,
            renamedItem.displayName);
    ReplaceRenamedIdentity(previousIdentity, renamedItem);
    if (!stateAccepted) {
        MessageDialog::Show(
            instance_, hwnd_,
            L"Windows 已完成重命名，但 Lattice 暂时无法接受配置更新。当前 Shell 名称保持不变；请不要退出并稍后重试。",
            L"Lattice 重命名",
            MB_OK | MB_ICONERROR);
    }
}

void DesktopSurfaceWindow::UpdateRenameEditGeometry() {
    if (renameEdit_ == nullptr) {
        return;
    }
    const auto item = std::find_if(
        visibleItems_.begin(), visibleItems_.end(),
        [&](const DesktopViewItem& value) {
            return IdentitiesEqual(value.path, renameIdentity_);
        });
    if (item == visibleItems_.end()) {
        FinishRename(false);
        return;
    }
    RECT label = LabelRect(*item);
    const int center = (label.left + label.right) / 2;
    const LONG width = (std::max<LONG>)(
        96, label.right - label.left + 24);
    label.left = center - width / 2;
    label.right = label.left + width;
    const RECT bounds = ClientBounds();
    if (label.left < bounds.left) {
        OffsetRect(&label, bounds.left - label.left, 0);
    }
    if (label.right > bounds.right) {
        OffsetRect(&label, bounds.right - label.right, 0);
    }
    label.bottom = (std::min<LONG>)(
        bounds.bottom,
        label.top + (std::max<LONG>)(
            24, label.bottom - label.top));
    POINT editOrigin{label.left, label.top};
    if (ClientToScreen(hwnd_, &editOrigin) == FALSE) {
        FinishRename(false);
        return;
    }
    SetWindowPos(
        renameEdit_, HWND_TOP,
        editOrigin.x, editOrigin.y,
        label.right - label.left,
        label.bottom - label.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

bool DesktopSurfaceWindow::FocusKeyboardWindow(
    HWND target) const noexcept {
    if (target == nullptr || IsWindow(target) == FALSE) {
        return false;
    }
    ShowWindow(target, SW_SHOWNORMAL);
    SetForegroundWindow(target);
    SetActiveWindow(target);
    SetFocus(target);
    GUITHREADINFO information{};
    information.cbSize = sizeof(information);
    return GetForegroundWindow() == target &&
        GetGUIThreadInfo(0, &information) != FALSE &&
        information.hwndFocus == target;
}

void DesktopSurfaceWindow::ReplaceRenamedIdentity(
    const std::wstring& previousIdentity,
    const DesktopShellRenameResult& renamedItem) {
    int systemImageIndex = -1;
    int overlayIndex = 0;
    bool foundShellImageIdentity = false;
    const auto replaceIdentity = [&](std::wstring& value) {
        if (IdentitiesEqual(value, previousIdentity)) {
            value = renamedItem.item.path;
        }
    };
    for (std::wstring& identity : assignedIdentities_) {
        replaceIdentity(identity);
    }
    for (PositionOverride& position : positionOverrides_) {
        replaceIdentity(position.identity);
    }
    const bool wasSelected = IsSelected(previousIdentity);
    selectedIdentities_.erase(previousIdentity);
    selectionBaseline_.erase(previousIdentity);
    replaceIdentity(pressedIdentity_);
    replaceIdentity(pendingRenameIdentity_);
    for (DesktopViewItem& item : snapshot_.items) {
        if (!IdentitiesEqual(item.path, previousIdentity)) {
            continue;
        }
        systemImageIndex = item.systemImageIndex;
        overlayIndex = item.overlayIndex;
        foundShellImageIdentity = true;
        item.path = renamedItem.item.path;
        item.displayName = renamedItem.displayName;
        item.shellChildPidl = renamedItem.item.desktopChildPidl;
    }
    if (!foundShellImageIdentity) {
        const auto currentItem = std::find_if(
            snapshot_.items.begin(), snapshot_.items.end(),
            [&](const DesktopViewItem& item) {
                return IdentitiesEqual(
                    item.path, renamedItem.item.path);
            });
        if (currentItem != snapshot_.items.end()) {
            systemImageIndex = currentItem->systemImageIndex;
            overlayIndex = currentItem->overlayIndex;
        }
    }
    if (wasSelected) {
        AddSelected(renamedItem.item.path);
    }
    RebuildVisibleItems();
    SynchronizeExplorerSelection();
    iconCache_.Alias(
        previousIdentity,
        renamedItem.item.path,
        systemImageIndex,
        overlayIndex,
        iconSize_);
    iconCache_.PreloadShellIcon(
        renamedItem.item.path,
        systemImageIndex,
        overlayIndex,
        iconSize_);
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DesktopSurfaceWindow::ApplyMarqueeSelection(
    const RECT& marqueeRect) {
    selectedIdentities_ = controlAtPointerDown_
        ? selectionBaseline_
        : IdentitySet{};
    for (size_t index = 0;
         index < visibleItems_.size(); ++index) {
        const DesktopViewItem& item = visibleItems_[index];
        const RECT interaction = InteractionRect(index);
        RECT intersection{};
        if (!IntersectRect(
                &intersection, &interaction, &marqueeRect)) {
            continue;
        }
        const bool selectedAtStart =
            selectionBaseline_.find(item.path) !=
                selectionBaseline_.end();
        if (controlAtPointerDown_ && selectedAtStart) {
            RemoveSelected(item.path);
        } else {
            AddSelected(item.path);
        }
    }
}

bool DesktopSurfaceWindow::IsAssigned(
    const std::wstring& identity) const {
    return std::any_of(
        assignedIdentities_.begin(),
        assignedIdentities_.end(),
        [&](const std::wstring& candidate) {
            return IdentitiesEqual(identity, candidate);
        });
}

void DesktopSurfaceWindow::RebuildVisibleItems() {
    CancelPointerCapture();
    visibleItems_.clear();
    visibleItems_.reserve(snapshot_.items.size());
    for (const DesktopViewItem& item : snapshot_.items) {
        if (!IsAssigned(item.path)) {
            visibleItems_.push_back(item);
        }
    }
    hoverIndex_ = -1;
    PruneSelectionToVisibleItems();
    RebuildInteractionRects();
    UpdateRenameEditGeometry();
}

void DesktopSurfaceWindow::SetItemScreenPoint(
    const std::wstring& identity,
    POINT screenPoint) {
    const auto item = std::find_if(
        snapshot_.items.begin(), snapshot_.items.end(),
        [&](const DesktopViewItem& value) {
            return IdentitiesEqual(value.path, identity);
        });
    if (item == snapshot_.items.end()) {
        return;
    }
    item->screenPoint = screenPoint;
    item->viewPoint = screenPoint;
    if (snapshot_.listViewWindow != nullptr &&
        IsWindow(snapshot_.listViewWindow) != FALSE) {
        ScreenToClient(snapshot_.listViewWindow, &item->viewPoint);
    }
}

bool DesktopSurfaceWindow::BuildShellDragImage(
    POINT sourceClientPoint,
    SHDRAGIMAGE& dragImage) {
    dragImage = {};
    struct DragPart {
        HICON icon = nullptr;
        RECT cell{};
        int height = 0;
    };
    std::vector<DragPart> parts;
    RECT bounds{};
    bool hasBounds = false;
    for (const DesktopViewItem& item : visibleItems_) {
        if (!IsSelected(item.path)) {
            continue;
        }
        HICON icon = iconCache_.CopyReadyIconForDrag(item.path);
        if (icon == nullptr) {
            for (const DragPart& part : parts) {
                DestroyIcon(part.icon);
            }
            return false;
        }
        const RECT cell = CellRect(item);
        const int extension = item.overlayIndex > 0
            ? std::max(1, iconSize_ / 14)
            : 0;
        parts.push_back(DragPart{
            icon, cell, iconSize_ + extension});
        if (!hasBounds) {
            bounds = cell;
            hasBounds = true;
        } else {
            UnionRect(&bounds, &bounds, &cell);
        }
    }
    if (!hasBounds || parts.empty()) {
        return false;
    }
    bounds.left = std::min(bounds.left, sourceClientPoint.x);
    bounds.top = std::min(bounds.top, sourceClientPoint.y);
    bounds.right = std::max(bounds.right, sourceClientPoint.x + 1);
    bounds.bottom = std::max(bounds.bottom, sourceClientPoint.y + 1);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int maxWidth = std::max(
        1,
        static_cast<int>(
            snapshot_.screenRect.right - snapshot_.screenRect.left));
    const int maxHeight = std::max(
        1,
        static_cast<int>(
            snapshot_.screenRect.bottom - snapshot_.screenRect.top));
    if (width <= 0 || height <= 0 ||
        width > maxWidth || height > maxHeight) {
        for (const DragPart& part : parts) {
            DestroyIcon(part.icon);
        }
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* blackPixels = nullptr;
    void* whitePixels = nullptr;
    HBITMAP blackBitmap = CreateDIBSection(
        nullptr, &bitmapInfo, DIB_RGB_COLORS,
        &blackPixels, nullptr, 0);
    HBITMAP whiteBitmap = CreateDIBSection(
        nullptr, &bitmapInfo, DIB_RGB_COLORS,
        &whitePixels, nullptr, 0);
    HDC blackDc = blackBitmap != nullptr
        ? CreateCompatibleDC(nullptr)
        : nullptr;
    HDC whiteDc = whiteBitmap != nullptr
        ? CreateCompatibleDC(nullptr)
        : nullptr;
    bool succeeded = blackPixels != nullptr && whitePixels != nullptr &&
        blackDc != nullptr && whiteDc != nullptr;
    HGDIOBJ oldBlack = nullptr;
    HGDIOBJ oldWhite = nullptr;
    if (succeeded) {
        oldBlack = SelectObject(blackDc, blackBitmap);
        oldWhite = SelectObject(whiteDc, whiteBitmap);
        succeeded = oldBlack != nullptr && oldBlack != HGDI_ERROR &&
            oldWhite != nullptr && oldWhite != HGDI_ERROR;
    }
    if (succeeded) {
        RECT bitmapBounds{0, 0, width, height};
        FillRect(
            blackDc,
            &bitmapBounds,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        FillRect(
            whiteDc,
            &bitmapBounds,
            static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        for (const DragPart& part : parts) {
            const int left = part.cell.left - bounds.left +
                (cellWidth_ - iconSize_) / 2;
            const int top = part.cell.top - bounds.top;
            if (DrawIconEx(
                    blackDc, left, top, part.icon,
                    iconSize_, part.height, 0, nullptr,
                    DI_NORMAL) == FALSE ||
                DrawIconEx(
                    whiteDc, left, top, part.icon,
                    iconSize_, part.height, 0, nullptr,
                    DI_NORMAL) == FALSE) {
                succeeded = false;
                break;
            }
        }
    }
    for (const DragPart& part : parts) {
        DestroyIcon(part.icon);
    }
    if (succeeded) {
        auto* black = static_cast<BYTE*>(blackPixels);
        const auto* white = static_cast<const BYTE*>(whitePixels);
        const size_t pixelCount =
            static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t index = 0; index < pixelCount; ++index) {
            BYTE* output = black + index * 4U;
            const BYTE* comparison = white + index * 4U;
            const int difference = std::max({
                static_cast<int>(comparison[0]) - output[0],
                static_cast<int>(comparison[1]) - output[1],
                static_cast<int>(comparison[2]) - output[2],
                0});
            output[3] = static_cast<BYTE>(255 -
                std::clamp(difference, 0, 255));
        }
    }
    if (oldBlack != nullptr && oldBlack != HGDI_ERROR) {
        SelectObject(blackDc, oldBlack);
    }
    if (oldWhite != nullptr && oldWhite != HGDI_ERROR) {
        SelectObject(whiteDc, oldWhite);
    }
    if (blackDc != nullptr) {
        DeleteDC(blackDc);
    }
    if (whiteDc != nullptr) {
        DeleteDC(whiteDc);
    }
    if (whiteBitmap != nullptr) {
        DeleteObject(whiteBitmap);
    }
    if (!succeeded) {
        if (blackBitmap != nullptr) {
            DeleteObject(blackBitmap);
        }
        return false;
    }
    dragImage.sizeDragImage = SIZE{width, height};
    dragImage.ptOffset = POINT{
        std::clamp(
            static_cast<int>(sourceClientPoint.x - bounds.left),
            0,
            width - 1),
        std::clamp(
            static_cast<int>(sourceClientPoint.y - bounds.top),
            0,
            height - 1)};
    dragImage.hbmpDragImage = blackBitmap;
    dragImage.crColorKey = CLR_NONE;
    return true;
}

bool DesktopSurfaceWindow::BeginInternalDragSession(
    POINT sourceClientPoint) {
    EndInternalDragSession();
#ifndef NDEBUG
    internalDropStage_ = InternalDropStage::None;
#endif
    if (hwnd_ == nullptr || IsWindow(hwnd_) == FALSE) {
        return false;
    }
    POINT sourceScreenPoint = sourceClientPoint;
    if (ClientToScreen(hwnd_, &sourceScreenPoint) == FALSE) {
        return false;
    }
    for (const DesktopViewItem& item : visibleItems_) {
        if (IsSelected(item.path)) {
            internalDragOriginalPositions_.push_back(
                DesktopPosition{item.path, item.screenPoint});
        }
    }
    if (internalDragOriginalPositions_.empty()) {
        return false;
    }
    internalDragSourceScreenPoint_ = sourceScreenPoint;
    internalDragActive_ = true;
#ifndef NDEBUG
    internalDropStage_ = InternalDropStage::SessionBegan;
#endif
    return true;
}

void DesktopSurfaceWindow::EndInternalDragSession() noexcept {
    internalDragActive_ = false;
    internalDragSourceScreenPoint_ = {};
    internalDragOriginalPositions_.clear();
}

std::vector<DesktopPosition>
DesktopSurfaceWindow::OffsetDragPositions(
    const std::vector<DesktopPosition>& originalPositions,
    POINT sourceScreenPoint,
    POINT dropScreenPoint) {
    const LONG deltaX = dropScreenPoint.x - sourceScreenPoint.x;
    const LONG deltaY = dropScreenPoint.y - sourceScreenPoint.y;
    std::vector<DesktopPosition> result;
    result.reserve(originalPositions.size());
    for (const DesktopPosition& original : originalPositions) {
        result.push_back(DesktopPosition{
            original.path,
            POINT{
                original.point.x + deltaX,
                original.point.y + deltaY}});
    }
    return result;
}

bool DesktopSurfaceWindow::PlanVisibleGridDrop(
    const std::vector<DesktopPosition>& visiblePositions,
    const std::vector<DesktopPosition>& selectedPositions,
    POINT sourceScreenPoint,
    POINT dropScreenPoint,
    const RECT& screenRect,
    int cellWidth,
    int cellHeight,
    int iconSize,
    std::vector<DesktopPosition>& plannedPositions) {
    plannedPositions.clear();
    if (visiblePositions.empty() || selectedPositions.empty() ||
        cellWidth <= 0 || cellHeight <= 0 || iconSize <= 0 ||
        screenRect.right <= screenRect.left ||
        screenRect.bottom <= screenRect.top) {
        return false;
    }

    using Cell = std::pair<int, int>;
    const POINT origin = selectedPositions.front().point;
    const auto cellForPoint = [&](POINT point) -> Cell {
        return Cell{
            static_cast<int>(std::lround(
                static_cast<double>(point.x - origin.x) /
                static_cast<double>(cellWidth))),
            static_cast<int>(std::lround(
                static_cast<double>(point.y - origin.y) /
                static_cast<double>(cellHeight)))};
    };
    const auto pointForCell = [&](Cell cell) -> POINT {
        return POINT{
            origin.x + cell.first * cellWidth,
            origin.y + cell.second * cellHeight};
    };

    const LONG horizontalInset = (cellWidth - iconSize) / 2;
    const LONG minimumAnchorX = screenRect.left + horizontalInset;
    const LONG maximumAnchorX =
        screenRect.right - cellWidth + horizontalInset;
    const LONG minimumAnchorY = screenRect.top;
    const LONG maximumAnchorY = screenRect.bottom - cellHeight;
    if (maximumAnchorX < minimumAnchorX ||
        maximumAnchorY < minimumAnchorY) {
        return false;
    }
    const int minimumColumn = static_cast<int>(std::ceil(
        static_cast<double>(minimumAnchorX - origin.x) /
        static_cast<double>(cellWidth)));
    const int maximumColumn = static_cast<int>(std::floor(
        static_cast<double>(maximumAnchorX - origin.x) /
        static_cast<double>(cellWidth)));
    const int minimumRow = static_cast<int>(std::ceil(
        static_cast<double>(minimumAnchorY - origin.y) /
        static_cast<double>(cellHeight)));
    const int maximumRow = static_cast<int>(std::floor(
        static_cast<double>(maximumAnchorY - origin.y) /
        static_cast<double>(cellHeight)));
    if (maximumColumn < minimumColumn || maximumRow < minimumRow) {
        return false;
    }

    IdentitySet selectedIdentities;
    std::map<std::wstring, POINT, IdentityLess> selectedOriginalPoints;
    std::vector<std::pair<std::wstring, Cell>> selectedCells;
    selectedCells.reserve(selectedPositions.size());
    for (const DesktopPosition& selected : selectedPositions) {
        const auto visible = std::find_if(
            visiblePositions.begin(), visiblePositions.end(),
            [&](const DesktopPosition& value) {
                return IdentitiesEqual(value.path, selected.path);
            });
        if (visible == visiblePositions.end() ||
            !selectedIdentities.insert(selected.path).second) {
            return false;
        }
        selectedOriginalPoints.emplace(selected.path, selected.point);
        selectedCells.emplace_back(
            selected.path, cellForPoint(selected.point));
    }

    int deltaColumn = static_cast<int>(std::lround(
        static_cast<double>(
            dropScreenPoint.x - sourceScreenPoint.x) /
        static_cast<double>(cellWidth)));
    int deltaRow = static_cast<int>(std::lround(
        static_cast<double>(
            dropScreenPoint.y - sourceScreenPoint.y) /
        static_cast<double>(cellHeight)));
    int minimumDeltaColumn = std::numeric_limits<int>::min();
    int maximumDeltaColumn = std::numeric_limits<int>::max();
    int minimumDeltaRow = std::numeric_limits<int>::min();
    int maximumDeltaRow = std::numeric_limits<int>::max();
    for (const DesktopPosition& selected : selectedPositions) {
        minimumDeltaColumn = (std::max)(
            minimumDeltaColumn,
            static_cast<int>(std::ceil(
                static_cast<double>(minimumAnchorX - selected.point.x) /
                static_cast<double>(cellWidth))));
        maximumDeltaColumn = (std::min)(
            maximumDeltaColumn,
            static_cast<int>(std::floor(
                static_cast<double>(maximumAnchorX - selected.point.x) /
                static_cast<double>(cellWidth))));
        minimumDeltaRow = (std::max)(
            minimumDeltaRow,
            static_cast<int>(std::ceil(
                static_cast<double>(minimumAnchorY - selected.point.y) /
                static_cast<double>(cellHeight))));
        maximumDeltaRow = (std::min)(
            maximumDeltaRow,
            static_cast<int>(std::floor(
                static_cast<double>(maximumAnchorY - selected.point.y) /
                static_cast<double>(cellHeight))));
    }
    if (minimumDeltaColumn > maximumDeltaColumn ||
        minimumDeltaRow > maximumDeltaRow) {
        return false;
    }
    deltaColumn = std::clamp(
        deltaColumn, minimumDeltaColumn, maximumDeltaColumn);
    deltaRow = std::clamp(
        deltaRow, minimumDeltaRow, maximumDeltaRow);

    std::map<std::wstring, Cell, IdentityLess> initialCells;
    std::map<Cell, std::vector<std::wstring>> occupants;
    for (const DesktopPosition& visible : visiblePositions) {
        const Cell cell = cellForPoint(visible.point);
        if (!initialCells.emplace(visible.path, cell).second) {
            return false;
        }
        if (selectedIdentities.find(visible.path) !=
            selectedIdentities.end()) {
            continue;
        }
        if (cell.first < minimumColumn ||
            cell.first > maximumColumn ||
            cell.second < minimumRow ||
            cell.second > maximumRow) {
            return false;
        }
        occupants[cell].push_back(visible.path);
    }

    std::map<std::wstring, Cell, IdentityLess> selectedTargets;
    std::set<Cell> reservedCells;
    for (const auto& selected : selectedCells) {
        const Cell target{
            selected.second.first + deltaColumn,
            selected.second.second + deltaRow};
        reservedCells.insert(target);
        selectedTargets.emplace(selected.first, target);
    }

    const long long columnCount =
        static_cast<long long>(maximumColumn) -
        static_cast<long long>(minimumColumn) + 1;
    const long long rowCount =
        static_cast<long long>(maximumRow) -
        static_cast<long long>(minimumRow) + 1;
    const long long cellCount = columnCount * rowCount;
    if (cellCount <= 0 ||
        static_cast<long long>(visiblePositions.size()) > cellCount) {
        return false;
    }
    const auto nextCell = [&](Cell cell) -> Cell {
        if (cell.second < maximumRow) {
            ++cell.second;
        } else {
            cell.second = minimumRow;
            if (cell.first < maximumColumn) {
                ++cell.first;
            } else {
                cell.first = minimumColumn;
            }
        }
        return cell;
    };

    for (const auto& selected : selectedCells) {
        const Cell target = selectedTargets.at(selected.first);
        auto occupied = occupants.find(target);
        if (occupied == occupants.end()) {
            continue;
        }
        std::vector<std::wstring> displaced =
            std::move(occupied->second);
        occupants.erase(occupied);
        for (std::wstring& displacedIdentity : displaced) {
            std::wstring carried = std::move(displacedIdentity);
            Cell candidate = nextCell(target);
            bool placed = false;
            for (long long attempt = 0; attempt < cellCount; ++attempt) {
                if (reservedCells.find(candidate) != reservedCells.end()) {
                    candidate = nextCell(candidate);
                    continue;
                }
                auto nextOccupied = occupants.find(candidate);
                if (nextOccupied == occupants.end()) {
                    occupants[candidate].push_back(std::move(carried));
                    placed = true;
                    break;
                }
                if (nextOccupied->second.empty()) {
                    return false;
                }
                std::swap(carried, nextOccupied->second.front());
                candidate = nextCell(candidate);
            }
            if (!placed) {
                return false;
            }
        }
    }

    std::map<std::wstring, Cell, IdentityLess> finalCells;
    for (const auto& occupant : occupants) {
        for (const std::wstring& identity : occupant.second) {
            finalCells.emplace(identity, occupant.first);
        }
    }
    finalCells.insert(selectedTargets.begin(), selectedTargets.end());
    if (finalCells.size() != visiblePositions.size()) {
        return false;
    }
    for (const DesktopPosition& visible : visiblePositions) {
        const auto final = finalCells.find(visible.path);
        if (final == finalCells.end()) {
            return false;
        }
        const auto initial = initialCells.find(visible.path);
        if (initial == initialCells.end()) {
            return false;
        }
        if (final->second == initial->second) {
            continue;
        }
        POINT point = pointForCell(final->second);
        const auto selectedOriginal =
            selectedOriginalPoints.find(visible.path);
        if (selectedOriginal != selectedOriginalPoints.end()) {
            point = POINT{
                selectedOriginal->second.x + deltaColumn * cellWidth,
                selectedOriginal->second.y + deltaRow * cellHeight};
        }
        plannedPositions.push_back(
            DesktopPosition{visible.path, point});
    }
    return true;
}

bool DesktopSurfaceWindow::CommitInternalDesktopDrop(
    POINT dropScreenPoint) {
    if (!internalDragActive_ || internalDragOriginalPositions_.empty()) {
        return false;
    }
    std::vector<DesktopPosition> visiblePositions;
    visiblePositions.reserve(visibleItems_.size());
    for (const DesktopViewItem& item : visibleItems_) {
        visiblePositions.push_back(
            DesktopPosition{item.path, item.screenPoint});
    }
    std::vector<DesktopPosition> planned;
    if (!PlanVisibleGridDrop(
            visiblePositions,
            internalDragOriginalPositions_,
            internalDragSourceScreenPoint_,
            dropScreenPoint,
            snapshot_.screenRect,
            cellWidth_,
            cellHeight_,
            iconSize_,
            planned)) {
#ifndef NDEBUG
        internalDropStage_ = InternalDropStage::PlanRejected;
#endif
        return false;
    }
    if (planned.empty()) {
#ifndef NDEBUG
        internalDropStage_ = InternalDropStage::PlannedNoChange;
#endif
        return true;
    }

    std::vector<DesktopPosition> viewPositions = planned;
    for (DesktopPosition& position : viewPositions) {
        if (snapshot_.listViewWindow == nullptr ||
            IsWindow(snapshot_.listViewWindow) == FALSE ||
            ScreenToClient(
                snapshot_.listViewWindow,
                &position.point) == FALSE) {
#ifndef NDEBUG
            internalDropStage_ =
                InternalDropStage::CoordinateRejected;
#endif
            return false;
        }
    }
    if (!displayPositionCommitHandler_ ||
        !displayPositionCommitHandler_(viewPositions)) {
#ifndef NDEBUG
        internalDropStage_ = InternalDropStage::CommitRejected;
#endif
        return false;
    }

    for (size_t index = 0; index < planned.size(); ++index) {
        const DesktopPosition& position = planned[index];
        auto existing = std::find_if(
            positionOverrides_.begin(), positionOverrides_.end(),
            [&](const PositionOverride& value) {
                return IdentitiesEqual(
                    value.identity, position.path);
            });
        if (existing == positionOverrides_.end()) {
            const auto nativeItem = std::find_if(
                snapshot_.items.begin(), snapshot_.items.end(),
                [&](const DesktopViewItem& value) {
                    return IdentitiesEqual(
                        value.path, position.path);
                });
            if (nativeItem == snapshot_.items.end()) {
                return false;
            }
            positionOverrides_.push_back(PositionOverride{
                position.path,
                position.point,
                nativeItem->screenPoint,
                viewPositions[index].point});
        } else {
            existing->screenPoint = position.point;
            existing->viewPoint = viewPositions[index].point;
        }
        SetItemScreenPoint(position.path, position.point);
    }
    RebuildVisibleItems();
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
#ifndef NDEBUG
    internalDropStage_ = InternalDropStage::Applied;
#endif
    return true;
}

void DesktopSurfaceWindow::MaintainDesktopLayer() {
    if (hwnd_ == nullptr || IsWindow(hwnd_) == FALSE) {
        return;
    }
    SetWindowPos(
        hwnd_,
        HWND_TOP,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
