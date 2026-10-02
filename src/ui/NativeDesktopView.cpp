#include "ui/NativeDesktopView.h"
#include <ShlObj.h>
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <utility>

namespace {
constexpr UINT kReconcile = WM_APP + 0x378;
constexpr UINT kShellRename = WM_APP + 0x379;
constexpr UINT_PTR kCreationWatchdog = 0x378;
constexpr wchar_t kClass[] = L"LatticeNativeDesktopView";
bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
}

NativeDesktopView::~NativeDesktopView() { Close(); }
ULONG NativeDesktopView::Release() {
    const ULONG count = --references_;
    if (!count) delete this;
    return count;
}
HRESULT NativeDesktopView::QueryInterface(REFIID iid, void** value) {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (iid != IID_IUnknown && iid != IID_IExplorerBrowserEvents) return E_NOINTERFACE;
    *value = static_cast<IExplorerBrowserEvents*>(this);
    AddRef(); return S_OK;
}
bool NativeDesktopView::Owns(HWND window) const {
    DWORD process = 0;
    return window && IsWindow(window) &&
        GetWindowThreadProcessId(window, &process) == thread_ && process == GetCurrentProcessId();
}
bool NativeDesktopView::Create(const DesktopViewSnapshot& source, HWND widgetWindow, IDropTarget* target,
    StateHandler stateHandler, DragHandler dragHandler, std::wstring& error) {
    Close(); closing_ = false;
    thread_ = GetCurrentThreadId(); sourceList_ = source.listViewWindow;
    if(!Owns(widgetWindow) || GetAncestor(widgetWindow,GA_PARENT)!=source.desktopHost) {
        error=L"原生桌面与格子宿主必须是同一线程的自有兄弟窗口。";
        return false;
    }
    widgetWindow_=widgetWindow;
    flags_ = source.viewFlags; iconSize_ = source.viewIconSize;
    DWORD_PTR spacing = 0, style = 0, uiState = 0;
    const auto readSource = [&](UINT message, DWORD_PTR& value) {
        if (SendMessageTimeoutW(sourceList_, message, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &value)) return true;
        error = L"无法读取原生桌面参数，消息=" + std::to_wstring(message) +
            L"，错误=" + std::to_wstring(GetLastError());
        return false;
    };
    if (!readSource(LVM_GETITEMSPACING, spacing) ||
        !readSource(LVM_GETEXTENDEDLISTVIEWSTYLE, style) ||
        !readSource(WM_QUERYUISTATE, uiState)) return false;
    spacing_ = static_cast<LRESULT>(spacing); extendedStyle_ = static_cast<DWORD>(style);
    uiState_=static_cast<DWORD>(uiState)&(UISF_HIDEFOCUS|UISF_HIDEACCEL);
    stateHandler_ = std::move(stateHandler); dragHandler_ = std::move(dragHandler); target_ = target;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    POINT origin{source.screenRect.left, source.screenRect.top};
    if (!ScreenToClient(source.desktopHost, &origin)) return false;
    const int width = source.screenRect.right - source.screenRect.left;
    const int height = source.screenRect.bottom - source.screenRect.top;
    parent_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClass,
        L"Lattice native desktop", WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        source.screenRect.left, source.screenRect.top, width, height, nullptr, nullptr, wc.hInstance, this);
    if (parent_) {
        SetLastError(ERROR_SUCCESS);
        const HWND previous=SetParent(parent_,source.desktopHost);
        if((!previous && GetLastError()!=ERROR_SUCCESS) || GetAncestor(parent_,GA_PARENT)!=source.desktopHost ||
            !SetWindowPos(parent_,nullptr,origin.x,origin.y,width,height,SWP_NOZORDER|SWP_NOACTIVATE)) {
            Close(); error=L"无法挂接Windows原生桌面承载。"; return false;
        }
    }
    HRESULT hr = parent_ ? CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(browser_.GetAddressOf())) : E_FAIL;
    const auto options = static_cast<EXPLORER_BROWSER_OPTIONS>(EBO_NOWRAPPERWINDOW | EBO_NOBORDER |
        EBO_NOTRAVELLOG | EBO_NOPERSISTVIEWSTATE | EBO_ALWAYSNAVIGATE);
    if (SUCCEEDED(hr)) hr = browser_->SetOptions(options);
    const RECT bounds{0, 0, width, height};
    const FOLDERSETTINGS settings{FVM_ICON, flags_ | FWF_DESKTOP | FWF_TRANSPARENT | FWF_NOBROWSERVIEWSTATE};
    if (SUCCEEDED(hr)) hr = browser_->Initialize(parent_, &bounds, &settings);
    if (SUCCEEDED(hr)) hr = browser_->SetOptions(options);
    Microsoft::WRL::ComPtr<IFolderViewOptions> folderOptions;
    if (SUCCEEDED(hr)) hr = browser_.As(&folderOptions);
    if (SUCCEEDED(hr)) hr = folderOptions->SetFolderViewOptions(FVO_CUSTOMPOSITION, FVO_CUSTOMPOSITION);
    if (SUCCEEDED(hr)) { hr = browser_->Advise(this, &cookie_); advised_ = SUCCEEDED(hr); }
    if (SUCCEEDED(hr)) {
        const ITEMIDLIST desktop{};
        const SHChangeNotifyEntry entry{&desktop,TRUE};
        shellNotification_=SHChangeNotifyRegister(parent_,SHCNRF_ShellLevel|SHCNRF_NewDelivery,
            SHCNE_RENAMEITEM|SHCNE_RENAMEFOLDER,kShellRename,1,&entry);
        if(!shellNotification_) hr=E_FAIL;
    }
    // FillFromObject walks a folder. Use the already existing executable as a
    // single file seed, then remove it before publishing any visible frame.
    // This also works when the user's Desktop is completely empty.
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    Microsoft::WRL::ComPtr<IShellItem> seed;
    if (SUCCEEDED(hr)) hr = length && length < ARRAYSIZE(executable)
        ? SHCreateItemFromParsingName(executable, nullptr, IID_PPV_ARGS(seed.GetAddressOf())) : E_FAIL;
    if (SUCCEEDED(hr)) {
        HRGN pendingRegion=CreateRectRgn(0,0,0,0);
        if (!pendingRegion || !SetWindowRgn(parent_,pendingRegion,FALSE)) {
            if(pendingRegion) DeleteObject(pendingRegion);
            hr=E_FAIL;
        } else ShowWindow(parent_,SW_SHOWNOACTIVATE);
    }
    if (SUCCEEDED(hr)) {
        SetTimer(parent_, kCreationWatchdog, 5000, nullptr); // Single deadline; no polling.
        hr = browser_->FillFromObject(seed.Get(), EBF_NONE);
    }
    if (FAILED(hr)) {
        error = L"无法建立Windows原生桌面视图，HRESULT=" + std::to_wstring(static_cast<long>(hr));
        Close(); return false;
    }
    error.clear(); return true;
}
void NativeDesktopView::Close() {
    if (closing_) return;
    closing_ = true;
    visible_ = false;
    if(shellNotification_) SHChangeNotifyDeregister(shellNotification_);
    shellNotification_=0;
    if (Owns(parent_)) { ShowWindow(parent_, SW_HIDE); KillTimer(parent_, kCreationWatchdog); }
    if (Owns(list_)) {
        if (registered_) RevokeDragDrop(list_);
        RemoveWindowSubclass(list_, ListProc, 78);
    }
    registered_ = false;
    if (Owns(viewWindow_)) RemoveWindowSubclass(viewWindow_, ViewProc, 78);
    if (browser_ && advised_) browser_->Unadvise(cookie_);
    advised_ = false;
    results_.Reset(); folderView_.Reset(); view_.Reset();
    if (browser_) browser_->Destroy();
    browser_.Reset(); target_.Reset();
    if (Owns(parent_)) DestroyWindow(parent_);
    parent_ = viewWindow_ = list_ = sourceList_ = nullptr;
    widgetWindow_=nullptr;
    ready_ = failed_ = queued_ = dragging_ = false; seedPending_ = true; paintFailed_ = false;
    membershipDirty_ = positionDirty_ = true;
    items_.clear(); restoreSelection_.clear(); stateHandler_ = {}; dragHandler_ = {};
    restoreFocus_.clear(); rebuildingMembership_=selectionRestorePending_=false;
    renameHandler_={};
}
void NativeDesktopView::Fail() {
    if (closing_ || failed_) return;
    failed_ = true; ready_ = false;
    if (Owns(parent_)) { ShowWindow(parent_, SW_HIDE); KillTimer(parent_, kCreationWatchdog); }
    if (stateHandler_) stateHandler_(false);
}
bool NativeDesktopView::Paint(HDC dc) {
    if (failed_) return false;
    if (!dc || !PaintDesktop(dc)) { paintFailed_ = true; Fail(); return false; }
    return true;
}
void NativeDesktopView::Show(bool visible) {
#ifndef NDEBUG
    if(visible) ++debugShows_; else ++debugHides_;
#endif
    visible_ = visible;
    if (!Owns(parent_)) return;
    if (visible && !Ready() && !failed_) {
        // Initial empty region prevents seed exposure while the Shell realizes
        // its view. Membership updates retain the previously visible frame.
        if (seedPending_) ShowWindow(parent_,SW_SHOWNOACTIVATE);
        return;
    }
    ShowWindow(parent_, visible && Ready() ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible && Ready()) {
        if (!registered_ && !InstallDropTarget()) return;
        SetWindowPos(parent_, HWND_TOP, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        RedrawWindow(parent_, nullptr, nullptr, RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
}
bool NativeDesktopView::InstallDropTarget() {
    if (!target_ || !Owns(list_)) { Fail(); return false; }
    RevokeDragDrop(list_);
    registered_ = SUCCEEDED(RegisterDragDrop(list_, target_.Get()));
    if (!registered_) Fail();
    return registered_;
}
void NativeDesktopView::RefreshWallpaper() {
    if (!Owns(parent_)) return;
    if (failed_ && paintFailed_) {
        HDC dc=GetDC(parent_);
        const bool recovered=dc && PaintDesktop(dc);
        if(dc) ReleaseDC(parent_,dc);
        if(!recovered) return;
        failed_=false; paintFailed_=false; positionDirty_=true;
        QueueReconcile();
    }
    RedrawWindow(parent_, nullptr, nullptr, RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
}
bool NativeDesktopView::Resize(const DesktopViewSnapshot& source) {
    if (!Owns(parent_) || !browser_ || GetAncestor(parent_, GA_PARENT) != source.desktopHost) {
        Fail(); return false;
    }
    POINT origin{source.screenRect.left, source.screenRect.top};
    RECT bounds{0, 0, source.screenRect.right-source.screenRect.left,
        source.screenRect.bottom-source.screenRect.top};
    if (!ScreenToClient(source.desktopHost, &origin) || bounds.right <= 0 || bounds.bottom <= 0 ||
        !SetWindowPos(parent_, nullptr, origin.x, origin.y, bounds.right, bounds.bottom,
            SWP_NOZORDER|SWP_NOACTIVATE) || FAILED(browser_->SetRect(nullptr, bounds))) {
        Fail(); return false;
    }
    RefreshWallpaper(); return true;
}
bool NativeDesktopView::TranslateMessage(MSG& message) {
    if (!Ready() || !view_ || message.message < WM_KEYFIRST || message.message > WM_KEYLAST ||
        !(message.hwnd==list_ || message.hwnd==viewWindow_ || message.hwnd==parent_ || IsChild(viewWindow_,message.hwnd))) return false;
    return view_->TranslateAccelerator(&message)==S_OK;
}
void NativeDesktopView::HandleShellRename(WPARAM wp,LPARAM lp) {
    PIDLIST_ABSOLUTE* pidls=nullptr; LONG event=0;
    const HANDLE lock=SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wp),static_cast<DWORD>(lp),&pidls,&event);
    if(!lock) return;
    std::wstring previous,replacement,name;
    PWSTR before=nullptr,after=nullptr,display=nullptr;
    if((event&(SHCNE_RENAMEITEM|SHCNE_RENAMEFOLDER)) && pidls && pidls[0] && pidls[1] &&
        SUCCEEDED(SHGetNameFromIDList(pidls[0],SIGDN_DESKTOPABSOLUTEPARSING,&before)) &&
        SUCCEEDED(SHGetNameFromIDList(pidls[1],SIGDN_DESKTOPABSOLUTEPARSING,&after)) &&
        SUCCEEDED(SHGetNameFromIDList(pidls[1],SIGDN_NORMALDISPLAY,&display))) {
        previous=before; replacement=after; name=display;
    }
    CoTaskMemFree(before); CoTaskMemFree(after); CoTaskMemFree(display);
    SHChangeNotification_Unlock(lock);
    if(closing_ || failed_ || previous.empty() || replacement.empty() || Same(previous,replacement) ||
        std::none_of(items_.begin(),items_.end(),[&](const DesktopPosition& p){return Same(p.path,previous);})) return;
    PreserveSelection();
    for(auto& p:restoreSelection_) if(Same(p,previous)) p=replacement;
    if(Same(restoreFocus_,previous)) restoreFocus_=replacement;
    for(auto& p:items_) if(Same(p.path,previous)) p.path=replacement;
    if(!renameHandler_ || !renameHandler_(previous,replacement,name)) { Fail(); return; }
    membershipDirty_=positionDirty_=true; QueueReconcile();
}
HRESULT NativeDesktopView::OnNavigationFailed(PCIDLIST_ABSOLUTE) { Fail(); return S_OK; }
HRESULT NativeDesktopView::OnViewCreated(IShellView* view) {
#ifndef NDEBUG
    ++debugViewsCreated_;
#endif
    if (closing_ || failed_) return E_ABORT;
    view_ = view;
    HRESULT hr = view ? view->GetWindow(&viewWindow_) : E_FAIL;
    if (SUCCEEDED(hr)) hr = view_.As(&folderView_);
    if (SUCCEEDED(hr)) hr = folderView_->GetFolder(IID_PPV_ARGS(results_.GetAddressOf()));
    if (SUCCEEDED(hr) && !Owns(viewWindow_)) hr = E_ACCESSDENIED;
    list_ = FindWindowExW(viewWindow_, nullptr, L"SysListView32", nullptr);
    if (SUCCEEDED(hr) && !Owns(list_)) hr = E_ACCESSDENIED;
    if (SUCCEEDED(hr)) hr = folderView_->SetViewModeAndIconSize(FVM_ICON, iconSize_);
    if (SUCCEEDED(hr)) {
        DWORD current = 0; hr = folderView_->GetCurrentFolderFlags(&current);
        if (SUCCEEDED(hr)) hr = folderView_->SetCurrentFolderFlags(current ^ flags_, flags_);
    }
    if (SUCCEEDED(hr)) {
        SendMessageW(list_, LVM_SETICONSPACING, 0, spacing_);
        SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, extendedStyle_);
        SendMessageW(parent_,WM_UPDATEUISTATE,MAKEWPARAM(UIS_SET,uiState_),0);
        SendMessageW(parent_,WM_UPDATEUISTATE,MAKEWPARAM(UIS_CLEAR,(UISF_HIDEFOCUS|UISF_HIDEACCEL)&~uiState_),0);
        if (!SetWindowSubclass(viewWindow_, ViewProc, 78, reinterpret_cast<DWORD_PTR>(this)) ||
            !SetWindowSubclass(list_, ListProc, 78, reinterpret_cast<DWORD_PTR>(this))) hr = E_FAIL;
    }
    if (FAILED(hr)) { Fail(); return hr; }
    view_->UIActivate(SVUIA_ACTIVATE_NOFOCUS);
    membershipDirty_ = positionDirty_ = true;
    QueueReconcile(); return S_OK;
}
std::wstring NativeDesktopView::IdentityAt(int index) const {
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR identity = nullptr;
    if (!folderView_ || FAILED(folderView_->GetItem(index, IID_PPV_ARGS(item.GetAddressOf()))) ||
        FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &identity))) return {};
    std::wstring result(identity); CoTaskMemFree(identity); return result;
}
std::vector<std::wstring> NativeDesktopView::SelectedIdentities() const {
    std::vector<std::wstring> result;
    if (!Owns(list_)) return result;
    for (int index = static_cast<int>(SendMessageW(list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
        index >= 0; index = static_cast<int>(SendMessageW(list_, LVM_GETNEXTITEM, index, LVNI_SELECTED))) {
        auto identity = IdentityAt(index);
        if (!identity.empty()) result.push_back(std::move(identity));
    }
    return result;
}
void NativeDesktopView::Update(const std::vector<DesktopPosition>& items) {
    bool changed = items.size() != items_.size();
    if (!changed) for (size_t i=0; i<items.size(); ++i) if (!Same(items[i].path, items_[i].path)) {
        changed = true; break;
    }
    items_ = items;
    membershipDirty_ = membershipDirty_ || changed; positionDirty_ = true;
    QueueReconcile();
}
void NativeDesktopView::QueueReconcile() {
    if (!closing_ && !failed_ && parent_ && !queued_) {
        queued_ = PostMessageW(parent_, kReconcile, 0, 0) != FALSE;
        if (!queued_) Fail();
    }
}
void NativeDesktopView::PreserveSelection() {
    if(selectionRestorePending_) return;
    restoreSelection_=SelectedIdentities();
    int focus=-1;
    restoreFocus_=folderView_ && SUCCEEDED(folderView_->GetFocusedItem(&focus)) && focus>=0 ? IdentityAt(focus) : std::wstring{};
    selectionRestorePending_=true;
}
void NativeDesktopView::Reconcile() {
    queued_ = false;
    if (!results_ || !folderView_ || closing_ || failed_ || dragging_) return;
    if (seedPending_) {
        int seedCount=0;
        if (FAILED(folderView_->ItemCount(SVGIO_ALLVIEW,&seedCount))) { Fail(); return; }
        if (!seedCount) return; // The seed insertion notification resumes us.
        if (seedCount!=1 || FAILED(browser_->RemoveAll())) { Fail(); return; }
        seedPending_=false;
    }
    if (membershipDirty_) {
#ifndef NDEBUG
        ++debugMembershipBuilds_;
#endif
        ready_ = false;
        PreserveSelection(); rebuildingMembership_=true;
        if (FAILED(results_->RemoveAll())) { Fail(); return; }
        membershipDirty_ = false;
        for (const auto& item : items_) {
            Microsoft::WRL::ComPtr<IShellItem> shellItem;
            if (FAILED(SHCreateItemFromParsingName(item.path.c_str(), nullptr,
                    IID_PPV_ARGS(shellItem.GetAddressOf()))) || FAILED(results_->AddItem(shellItem.Get()))) {
                Fail(); return;
            }
        }
    }
    int count = -1;
    if (FAILED(folderView_->ItemCount(SVGIO_ALLVIEW, &count))) { Fail(); return; }
    if (count != static_cast<int>(items_.size())) return; // Insert/delete notification resumes us.
    std::vector<PITEMID_CHILD> children(items_.size(), nullptr);
    std::vector<POINT> points(items_.size());
    bool valid = true;
    for (int index=0; index<count; ++index) {
        const auto identity = IdentityAt(index);
        const auto found = std::find_if(items_.begin(), items_.end(),
            [&](const DesktopPosition& value) { return Same(value.path, identity); });
        if (found == items_.end() || FAILED(folderView_->Item(index, &children[index]))) { valid=false; break; }
        points[index] = found->point;
        if (!ScreenToClient(list_, &points[index])) { valid=false; break; }
    }
    if (valid && positionDirty_ && count) {
#ifndef NDEBUG
        ++debugPositionApplications_;
#endif
        valid = SUCCEEDED(folderView_->SelectAndPositionItems(
            static_cast<UINT>(count), const_cast<PCUITEMID_CHILD_ARRAY>(children.data()), points.data(), SVSI_NOSTATECHANGE));
    }
    for (int index=0; selectionRestorePending_ && valid && index<count; ++index) {
        const auto identity=IdentityAt(index);
        const bool selected=std::any_of(restoreSelection_.begin(), restoreSelection_.end(),
            [&](const std::wstring& value) { return Same(value,identity); });
        const bool focused=!restoreFocus_.empty() && Same(restoreFocus_,identity);
        if(selectionRestorePending_ && (selected || focused))
            valid=SUCCEEDED(view_->SelectItem(children[index],SVSI_NOTAKEFOCUS|
                (selected?SVSI_SELECT:0)|(focused?SVSI_FOCUSED:0)));
    }
    for (auto* child : children) CoTaskMemFree(child);
    if (!valid) { Fail(); return; }
    restoreSelection_.clear(); positionDirty_ = false;
    restoreFocus_.clear(); selectionRestorePending_=rebuildingMembership_=false;
    const bool firstReady = !ready_; ready_ = true;
    KillTimer(parent_, kCreationWatchdog);
    if (firstReady) {
        ShowWindow(parent_,SW_HIDE);
        if(!SetWindowRgn(parent_,nullptr,FALSE)) { Fail(); return; }
    }
    if (firstReady && visible_) Show(true);
    // The owner raises its widget sibling last; later position updates do not change z-order.
    if (firstReady && stateHandler_) stateHandler_(true);
}
LRESULT CALLBACK NativeDesktopView::WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<NativeDesktopView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<NativeDesktopView*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->parent_ = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wp, lp);
    if(message==WM_WINDOWPOSCHANGING && lp && self->Owns(self->widgetWindow_) &&
        GetAncestor(self->widgetWindow_,GA_PARENT)==GetAncestor(hwnd,GA_PARENT)) {
        auto* position=reinterpret_cast<WINDOWPOS*>(lp);
        if((position->flags&SWP_NOZORDER)==0) position->hwndInsertAfter=self->widgetWindow_;
    }
    if (message == kReconcile) { self->Reconcile(); return 0; }
    if (message == kShellRename) { self->HandleShellRename(wp,lp); return 0; }
    if (message == WM_TIMER && wp == kCreationWatchdog) { self->Fail(); return 0; }
    if (message == WM_ERASEBKGND || message == WM_PRINTCLIENT) return self->Paint(reinterpret_cast<HDC>(wp));
    if (message == WM_PAINT) {
#ifndef NDEBUG
        ++self->debugParentPaints_;
#endif
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); self->Paint(dc); EndPaint(hwnd, &ps); return 0;
    }
    if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED || message == WM_DISPLAYCHANGE)
        self->RefreshWallpaper();
    return DefWindowProcW(hwnd, message, wp, lp);
}
LRESULT CALLBACK NativeDesktopView::ViewProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR reference) {
    auto& self = *reinterpret_cast<NativeDesktopView*>(reference);
    if (message == WM_ERASEBKGND) return self.Paint(reinterpret_cast<HDC>(wp));
    if (message == WM_NOTIFY && lp) {
        const auto* notice = reinterpret_cast<NMHDR*>(lp);
        if (notice->hwndFrom == self.list_) {
            if(notice->code==LVN_DELETEALLITEMS && !self.seedPending_ &&
                !self.rebuildingMembership_ && !self.closing_ && !self.failed_) {
                self.PreserveSelection();
                self.membershipDirty_=self.positionDirty_=true;
            }
            if (notice->code == LVN_INSERTITEM || notice->code == LVN_DELETEITEM || notice->code == LVN_DELETEALLITEMS)
                self.QueueReconcile();
            if (notice->code == LVN_BEGINDRAG || notice->code == LVN_BEGINRDRAG) {
                if (!self.InstallDropTarget()) return 0;
                self.dragging_ = true;
                if (self.dragHandler_) self.dragHandler_(self.SelectedIdentities(), self.pressedScreen_, true);
                const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
                self.dragging_ = false;
                if (self.dragHandler_) self.dragHandler_({}, self.pressedScreen_, false);
                if (self.membershipDirty_ || self.positionDirty_) self.QueueReconcile();
                return result;
            }
        }
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}
LRESULT CALLBACK NativeDesktopView::ListProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR reference) {
    auto& self = *reinterpret_cast<NativeDesktopView*>(reference);
    if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) {
        self.pressedScreen_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &self.pressedScreen_);
    }
    // DefView can clear its results internally on Refresh without sending
    // ListView deletion messages. Its ensuing paint is the existing event
    // boundary; validate only the O(1) row count, then reuse the projection.
    if(message==WM_PAINT && self.ready_ && !self.rebuildingMembership_ &&
        !self.closing_ && !self.failed_ &&
        SendMessageW(hwnd,LVM_GETITEMCOUNT,0,0)!=static_cast<LRESULT>(self.items_.size())) {
#ifndef NDEBUG
        ++self.debugPaintRecoveries_;
#endif
        self.PreserveSelection();
        self.membershipDirty_=self.positionDirty_=true;
    }
    const LRESULT result=DefSubclassProc(hwnd, message, wp, lp);
    if (message==LVM_SETITEMCOUNT || message==LVM_INSERTITEMW || message==LVM_DELETEITEM || message==LVM_DELETEALLITEMS ||
        (message==WM_PAINT && (self.seedPending_ || self.membershipDirty_ || self.positionDirty_))) self.QueueReconcile();
    return result;
}
