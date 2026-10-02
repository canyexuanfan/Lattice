#pragma once

#include <Windows.h>
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "desktop/DesktopLayout.h"

// Owns only windows created on the calling UI thread. Never borrows an
// Explorer window for subclassing, destruction, selection or position writes.
class NativeDesktopView final : public IExplorerBrowserEvents {
public:
    using StateHandler = std::function<void(bool)>;
    using DragHandler = std::function<void(const std::vector<std::wstring>&, POINT, bool)>;
    using RenameHandler = std::function<bool(const std::wstring&, const std::wstring&, const std::wstring&)>;
    NativeDesktopView() = default;
    bool Create(const DesktopViewSnapshot& source, HWND widgetWindow, IDropTarget* target,
        StateHandler stateHandler, DragHandler dragHandler, std::wstring& error);
    void Close();
    void Show(bool visible);
    void Update(const std::vector<DesktopPosition>& items);
    void RefreshWallpaper();
    bool Resize(const DesktopViewSnapshot& source);
    bool TranslateMessage(MSG& message);
    void SetRenameHandler(RenameHandler handler) { renameHandler_=std::move(handler); }
    bool Ready() const noexcept { return ready_ && !failed_; }
    HWND Window() const noexcept { return parent_; }
    HWND ListWindow() const noexcept { return list_; }
    std::vector<std::wstring> SelectedIdentities() const;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE OnNavigationPending(PCIDLIST_ABSOLUTE) override {
#ifndef NDEBUG
        ++debugNavigationRequests_;
#endif
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnViewCreated(IShellView* view) override;
    HRESULT STDMETHODCALLTYPE OnNavigationComplete(PCIDLIST_ABSOLUTE) override { QueueReconcile(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnNavigationFailed(PCIDLIST_ABSOLUTE) override;

private:
    friend struct DesktopSurfaceWindowSmokeAccess;
    ~NativeDesktopView();
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK ListProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void QueueReconcile();
    void PreserveSelection();
    void Reconcile();
    void Fail();
    bool Paint(HDC dc);
    bool InstallDropTarget();
    void HandleShellRename(WPARAM, LPARAM);
    bool Owns(HWND window) const;
    std::wstring IdentityAt(int index) const;

    std::atomic<ULONG> references_{1};
    Microsoft::WRL::ComPtr<IExplorerBrowser> browser_;
    Microsoft::WRL::ComPtr<IShellView> view_;
    Microsoft::WRL::ComPtr<IFolderView2> folderView_;
    Microsoft::WRL::ComPtr<IResultsFolder> results_;
    Microsoft::WRL::ComPtr<IDropTarget> target_;
    HWND parent_ = nullptr, viewWindow_ = nullptr, list_ = nullptr;
    HWND sourceList_ = nullptr;
    HWND widgetWindow_ = nullptr;
    DWORD thread_ = 0, flags_ = 0, extendedStyle_ = 0, cookie_ = 0, uiState_ = 0;
    ULONG shellNotification_ = 0;
    int iconSize_ = 0;
    LRESULT spacing_ = 0;
    bool advised_ = false, ready_ = false, failed_ = false, visible_ = false;
    bool queued_ = false, membershipDirty_ = true, positionDirty_ = true, closing_ = false;
    bool dragging_ = false, registered_ = false;
    bool seedPending_ = true;
    bool paintFailed_ = false;
    bool rebuildingMembership_ = false, selectionRestorePending_ = false;
#ifndef NDEBUG
    unsigned debugNavigationRequests_=0, debugViewsCreated_=0, debugMembershipBuilds_=0;
    unsigned debugPaintRecoveries_=0, debugShows_=0, debugHides_=0, debugPositionApplications_=0;
    unsigned debugParentPaints_=0;
#endif
    POINT pressedScreen_{};
    std::vector<DesktopPosition> items_;
    std::vector<std::wstring> restoreSelection_;
    std::wstring restoreFocus_;
    StateHandler stateHandler_;
    DragHandler dragHandler_;
    RenameHandler renameHandler_;
};
