#include "testing/DisplayModesSmoke.h"
#include "testing/SmokeCommands.h"
#include "ui/MainWindow.h"
#include "config/LayoutSnapshot.h"
#include "shell/ShellDragDrop.h"
#include "shell/ShellDropTarget.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <windowsx.h>
#include <dwmapi.h>
#include <ShlObj.h>

struct DisplayModesSmokeAccess {
    static void Mode(MainWindow& w, int mode) { w.SetViewMode(mode); }
    static DesktopSurfaceWindow* Surface(MainWindow& w) { return w.desktopSurface_.get(); }
    static WindowConfig Active(MainWindow& w) { return w.windowConfig_; }
    static void SaveWindow(MainWindow& w) { w.SaveWindowConfig(); }
    static RECT Grid(MainWindow& w) {return w.GridBounds();}
    static RECT Tab(MainWindow& w,size_t n) {return w.TabBounds(n);}
    static RECT Viewport(MainWindow& w) {return w.TabViewport();}
    static std::wstring Title(MainWindow& w) {return Surface(w)->hostedWidgets_.front()->descriptor.title;}
    static void Hide(MainWindow& w) { w.ToggleAllVisible(); }
    static void Settings(MainWindow& w) {w.ShowSettings();}
    static void Temporary(MainWindow& w) {
        w.ToggleTemporaryAccess();
        std::cout<<"temporary_state active="<<w.temporaryAccess_<<" promoted="<<w.temporaryPromoted_<<" busy="<<w.accessInteractionDepth_<<" pending="<<w.temporaryEndPending_<<std::endl;
    }
    static bool TemporaryActive(MainWindow& w) {return w.temporaryAccess_&&w.temporaryPromoted_;}
    static void OpenTrace(MainWindow& w,size_t count) {auto* s=Surface(w);std::cout<<"open_input down="<<s->leftDownMessageCountForSmoke_<<" up="<<s->leftUpMessageCountForSmoke_<<" double="<<s->doubleClickMessageCountForSmoke_<<" count="<<count<<std::endl;}
    static size_t DragStarts(MainWindow& w) {return Surface(w)->shellDragStartCount_;}
    static size_t DoubleClicks(MainWindow& w) {return Surface(w)->doubleClickMessageCountForSmoke_;}
    static size_t LeftDowns(MainWindow& w) {return Surface(w)->leftDownMessageCountForSmoke_;}
    static bool ContainerMoving(MainWindow& w) {return Surface(w)->hostedPointerGesture_==DesktopSurfaceWindow::HostedPointerGesture::Moving;}
    static bool EndPending(MainWindow& w) {return w.temporaryEndPending_;}
    static HWND Native(MainWindow& w) {return Surface(w)->nativeDesktop_->Window();}
    static void SaveMetadata(MainWindow& w) {w.SaveOrganizerConfig();}
    static bool Bind(MainWindow& w,const AppSettings& settings,std::wstring& error) {return w.ConfigureAccessHotkeys(settings,error);}
    static int BindingId(MainWindow& w,int role) {return w.accessHotkeys_[static_cast<size_t>(role)].id;}
    static int BindingKey(MainWindow& w,int role) {return w.accessHotkeys_[static_cast<size_t>(role)].key;}
    static void FireHide(MainWindow& w) {SendMessageW(w.Window(),WM_HOTKEY,static_cast<WPARAM>(BindingId(w,0)),0);}

    static void CreateCategory(MainWindow& w) {w.CreateCategory();}
    static void RenameCategory(MainWindow& w) {w.RenameCurrentCategory();}
    static void DeleteCategory(MainWindow& w) {w.DeleteCurrentCategory();}
    static std::wstring Current(MainWindow& w) {return w.organizerConfig_.currentCategoryId;}
    static std::wstring Name(MainWindow& w) {return w.CurrentCategoryName();}
    static size_t Count(MainWindow& w) {return w.organizerConfig_.categories.size();}
    static void Overflow(MainWindow& w,bool add) {
        if(add)for(int i=0;i<12;++i) {::Category c;c.id=L"overflow-"+std::to_wstring(i);c.name=L"标签"+std::to_wstring(i);c.layout=w.organizerConfig_.window;w.organizerConfig_.categories.push_back(c);}
        else std::erase_if(w.organizerConfig_.categories,[](const auto& c){return c.id.starts_with(L"overflow-");});
        w.SaveOrganizerConfig();w.RefreshCurrentItems();
    }
    static IDropTarget* DropTarget(MainWindow& w) {return Surface(w)->desktopDropTarget_.Get();}
    static bool CollectionsDone(MainWindow& w) {return w.hostedCollectionsPending_==0;}
    static size_t Members(MainWindow& w,const wchar_t* id) {return w.FindCategory(id)->itemIds.size();}
    static void Reorder(MainWindow& w) {NeedReorder(w);}
    static void NeedReorder(MainWindow& w) {w.ReorderHostedSelection(L"first",{L"p82-0",L"p82-1"},0);}

    static void Category(MainWindow& w, const wchar_t* id) {
        w.organizerConfig_.currentCategoryId=id; w.RefreshCurrentItems(); w.SaveOrganizerConfig();
    }
    static bool Ready(MainWindow& w) {
        auto* s=Surface(w);
        return s && s->nativeDesktop_ && s->nativeDesktop_->Ready();
    }
    static WidgetView& View(MainWindow& w) {
        return Surface(w)->hostedWidgets_.front()->view;
    }
    static WidgetView& AccessView(MainWindow& w) {
        for(auto& hosted:Surface(w)->hostedWidgets_)if(hosted->descriptor.categoryId==L"first")return hosted->view;
        throw std::runtime_error("temporary source category absent");
    }
    static POINT ItemPoint(MainWindow& w,size_t n) {return PaintedPoint(View(w),n);}
    static POINT AccessPoint(MainWindow& w,size_t n) {
        auto& view=AccessView(w);const RECT cell=view.ItemHostCell(n);
        for(int y=cell.top+8;y<cell.bottom-8;y+=2) for(int x=cell.left+8;x<cell.right-8;x+=2) {
            const auto hit=view.HitTestHostPoint(POINT{x,y});
            const auto inset=view.HitTestHostPoint(POINT{x+6,y+6});
            if(hit.kind==WidgetViewHitKind::ItemIcon&&hit.itemIndex==static_cast<int>(n)&&
               inset.kind==WidgetViewHitKind::ItemIcon&&inset.itemIndex==static_cast<int>(n))return POINT{x+3,y+3};
        }
        throw std::runtime_error("interior temporary icon point absent");
    }
    static POINT PaintedPoint(WidgetView& view, size_t n) { const RECT cell=view.ItemHostCell(n);
        for(int y=cell.top;y<cell.bottom;y+=2) for(int x=cell.left;x<cell.right;x+=2) {
            const auto hit=view.HitTestHostPoint(POINT{x,y});
            if(hit.kind==WidgetViewHitKind::ItemIcon && hit.itemIndex==static_cast<int>(n)) return POINT{x,y};
        }
        throw std::runtime_error("painted item point absent");
    }
    static void Refresh(MainWindow& w) { w.SyncHostedWidgets(); }
    static void ContinuePointer(MainWindow& w,POINT point) {Surface(w)->ContinueHostedPointerGesture(point);}
    static void ResetPointer(MainWindow& w) {Surface(w)->ResetHostedPointerGesture(); if(GetCapture()==Surface(w)->Window())ReleaseCapture();}
    static size_t NoopPreviews(MainWindow& w) {
        auto* surface=Surface(w);const auto original=surface->hostedLayoutPreviewHandler_;size_t calls=0;
        surface->SetHostedLayoutPreviewHandler([&](RECT rect){++calls;if(original)original(rect);});
        const POINT same=surface->hostedPointerCurrent_;
        for(int n=0;n<100;++n)surface->ContinueHostedPointerGesture(same);
        surface->SetHostedLayoutPreviewHandler(original);return calls;
    }
    static WindowConfig GridOracle(MainWindow& w, const WindowConfig& tabs) {
        auto* category=w.FindCategory(L"first");const auto saved=category->layout;
        category->layout=tabs;
        Mode(w,1); w.hostedWidgetCategoryIds_={L"first"}; Refresh(w);
        return saved;
    }
    static void EndOracle(MainWindow& w,const WindowConfig& saved) {
        w.FindCategory(L"first")->layout=saved;w.SaveOrganizerConfig();Mode(w,0);
    }
    static void Fault(MainWindow& w) { Surface(w)->StartWallpaperRecovery(true); }
};
namespace {
void Need(bool good,const char* label) { if(!good) throw std::runtime_error(label); }
void PumpUntil(const std::function<bool()>& ready) {
    const ULONGLONG until=GetTickCount64()+15000;
    while(!ready()&&GetTickCount64()<until) {
        MSG msg{};
        for(int batch=0;batch<64&&PeekMessageW(&msg,nullptr,0,0,PM_REMOVE);++batch) {
            if(msg.message==WM_QUIT) continue;
            TranslateMessage(&msg);DispatchMessageW(&msg);
            if(ready()) return;
        }
        MsgWaitForMultipleObjects(0,nullptr,FALSE,20,QS_ALLINPUT);
    }
    Need(ready(),"bounded ready timeout");
}
void Click(HWND h,POINT p,bool ctrl=false) {
    SendMessageW(h,WM_LBUTTONDOWN,MK_LBUTTON|(ctrl?MK_CONTROL:0),MAKELPARAM(p.x,p.y));
    SendMessageW(h,WM_LBUTTONUP,ctrl?MK_CONTROL:0,MAKELPARAM(p.x,p.y));
}
bool renameSeen=false,menuSeen=false;int cancelKind=0;
MainWindow* accessMenuMain=nullptr;bool accessMenuDeferred=false;
int modalAction=0;bool modalSeen=false,modalConflictKept=false,modalImage=false;const wchar_t* modalText=L"";
void CaptureKey(HWND field,int key) {
    BYTE before[256]{},state[256]{};GetKeyboardState(before);CopyMemory(state,before,sizeof(state));
    state[VK_CONTROL]=state[VK_MENU]=0x80;SetKeyboardState(state);
    SendMessageW(field,WM_KEYDOWN,static_cast<WPARAM>(key),0);SetKeyboardState(before);
}
bool CaptureOwnedSettings(HWND h,const std::filesystem::path& path) {
    RECT bounds{};MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
    if(!GetWindowRect(h,&bounds)||!GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST),&monitor))return false;
    if(bounds.left<monitor.rcWork.left||bounds.top<monitor.rcWork.top||bounds.right>monitor.rcWork.right||bounds.bottom>monitor.rcWork.bottom)return false;
    const bool topmost=(GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
    if(!SetWindowPos(h,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE))return false;
    RedrawWindow(h,nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_INVALIDATE);DwmFlush();
    const POINT point{bounds.left+(bounds.right-bounds.left)/2,bounds.top+12};
    const HWND hit=WindowFromPoint(point);
    const bool result=(hit==h||IsChild(h,hit))&&SaveSmokeWindowScreen(h,path.wstring());
    if(!topmost)SetWindowPos(h,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    return result;
}
VOID CALLBACK CompleteModal(HWND,UINT,UINT_PTR timer,DWORD) {
    const wchar_t* cls=(modalAction<=2||modalAction>=5)?L"Lattice.SettingsDialog":modalAction==3?L"Lattice.InputDialog":L"Lattice.MessageDialog";
    HWND h=FindWindowW(cls,nullptr);if(!h)return;
    modalSeen=true;
    if(modalAction>=5) {
        wchar_t directory[32768]{};GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",directory,ARRAYSIZE(directory));
        if(modalAction==5) {
            CaptureKey(GetDlgItem(h,1012),VK_F21);CaptureKey(GetDlgItem(h,1013),VK_F22);
            SendMessageW(h,WM_COMMAND,1007,0);modalConflictKept=IsWindow(h)!=FALSE;
            if(modalConflictKept) {RedrawWindow(h,nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_INVALIDATE);DwmFlush();
                modalImage=CaptureOwnedSettings(h,std::filesystem::path(directory)/L"settings-hotkey-conflict.bmp");
                SendMessageW(h,WM_COMMAND,1008,0);
            }
        } else {
            CaptureKey(GetDlgItem(h,1012),VK_F24);
            wchar_t captured[80]{};GetWindowTextW(GetDlgItem(h,1012),captured,ARRAYSIZE(captured));
            modalImage=std::wstring(captured)==L"Ctrl + Alt + F24";
            SendMessageW(GetDlgItem(h,1013),WM_KEYDOWN,VK_DELETE,0);
            RedrawWindow(h,nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_INVALIDATE);DwmFlush();
            modalImage=CaptureOwnedSettings(h,std::filesystem::path(directory)/L"settings-hotkey-normal.bmp")&&modalImage;
            SendMessageW(h,WM_COMMAND,1007,0);
        }
    } else if(modalAction<=2) {
        SendMessageW(GetDlgItem(h,1011),CB_SETCURSEL,0,0);
        SendMessageW(h,WM_COMMAND,modalAction==1?1008:1007,0);
    } else if(modalAction==3) {
        SetWindowTextW(GetDlgItem(h,1001),modalText);SendMessageW(h,WM_COMMAND,IDOK,0);
    } else SendMessageW(h,WM_COMMAND,IDYES,0);
    KillTimer(nullptr,timer);
}
POINT accessDropPoint{};int accessDragStep=0;bool accessDragCancel=false;
void MouseInput(POINT point,DWORD flags) {
    INPUT input{};input.type=INPUT_MOUSE;
    const int left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
    input.mi.dx=MulDiv(point.x-left,65535,std::max(1,GetSystemMetrics(SM_CXVIRTUALSCREEN)-1));
    input.mi.dy=MulDiv(point.y-top,65535,std::max(1,GetSystemMetrics(SM_CYVIRTUALSCREEN)-1));
    input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|flags;
    Need(SendInput(1,&input,sizeof(input))==1,"owned mouse input accepted");
}
VOID CALLBACK DriveAccessDrag(HWND,UINT,UINT_PTR timer,DWORD) {
    if(accessDragCancel) {
        INPUT input{};input.type=INPUT_KEYBOARD;input.ki.wVk=VK_ESCAPE;
        input.ki.dwFlags=accessDragStep?KEYEVENTF_KEYUP:0;SendInput(1,&input,sizeof(input));
        if(accessDragStep++)MouseInput(accessDropPoint,MOUSEEVENTF_LEFTUP);
    } else MouseInput(accessDropPoint,accessDragStep++?MOUSEEVENTF_LEFTUP:0);
    if(accessDragStep>1)KillTimer(nullptr,timer);
}
void PrepareAccessOpen(const std::filesystem::path& root,AppConfig& cfg) {
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,ARRAYSIZE(module));
    const auto project=std::filesystem::path(module).parent_path().parent_path().parent_path();
    const auto target=project/L".workspace/build/s0-open-recipient/S0OpenRecipient.exe";
    Need(std::filesystem::exists(target),"existing controlled recipient exists");
    Microsoft::WRL::ComPtr<IShellLinkW> link;Microsoft::WRL::ComPtr<IPersistFile> file;
    Need(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(link.GetAddressOf()))),"controlled link create");
    const auto arguments=L"--receipt-directory \""+root.wstring()+L"\" --token 00000000000000000000000000000082";
    Need(SUCCEEDED(link->SetPath(target.c_str()))&&SUCCEEDED(link->SetArguments(arguments.c_str()))&&SUCCEEDED(link->SetWorkingDirectory(root.c_str()))&&SUCCEEDED(link.As(&file)),"controlled link fields");
    cfg.items[2].path=(root/L"p82-controlled-open.lnk").wstring();cfg.items[2].displayName=L"受控打开";
    Need(SUCCEEDED(file->Save(cfg.items[2].path.c_str(),TRUE)),"controlled link save");
}
size_t AccessOpenCount(const std::filesystem::path& root) {
    size_t count=0;for(const auto& entry:std::filesystem::directory_iterator(root))if(entry.path().filename().wstring().starts_with(L"recipient-00000000000000000000000000000082-")&&entry.path().extension()==L".json"&&!entry.path().filename().wstring().ends_with(L".started.json"))++count;return count;
}
void PumpForInput(ULONGLONG duration) {const auto end=GetTickCount64()+duration;PumpUntil([&]{return GetTickCount64()>=end;});}
void Modal(const std::function<void()>& action,int kind,const wchar_t* value=L"") {
    modalAction=kind;modalText=value;modalSeen=false;
    const UINT_PTR timer=SetTimer(nullptr,0,100,CompleteModal);action();KillTimer(nullptr,timer);
    Need(modalSeen,"actual production modal reached");
}
VOID CALLBACK CancelUi(HWND,UINT,UINT_PTR timer,DWORD) {
    if(cancelKind==1) {HWND h=FindWindowW(L"Lattice.InputDialog",nullptr);if(h) {renameSeen=true;SendMessageW(h,WM_COMMAND,IDCANCEL,0);KillTimer(nullptr,timer);}}
    else if(FindWindowW(L"#32768",nullptr)) {
        menuSeen=true;
        if(accessMenuMain) {DisplayModesSmokeAccess::Temporary(*accessMenuMain);accessMenuDeferred=DisplayModesSmokeAccess::TemporaryActive(*accessMenuMain)&&DisplayModesSmokeAccess::EndPending(*accessMenuMain);}
        EndMenu();KillTimer(nullptr,timer);
    }
}
void CancelledKey(HWND h,UINT key,int kind) {
    cancelKind=kind;const UINT_PTR timer=SetTimer(nullptr,0,100,CancelUi);
    SendMessageW(h,WM_KEYDOWN,key,0);KillTimer(nullptr,timer);
}
bool SameRect(const WindowConfig& a,const WindowConfig& b) {
    return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height&&a.locked==b.locked&&a.collapsed==b.collapsed;
}
void Seed(ConfigStore& store,const std::filesystem::path& root) {
    AppConfig cfg=store.LoadAppConfig();cfg.settings.restoreHiddenState=true;cfg.settings.lastVisible=true;
    cfg.settings.quickHideKey=cfg.settings.temporaryAccessKey=0;
    cfg.settings.showPublicDesktopItems=false;cfg.window.x=70;cfg.window.y=70;
    cfg.window.width=380;cfg.window.height=390;cfg.window.locked=true;cfg.window.viewMode=1;
    cfg.tabContainer=cfg.window;cfg.tabContainer.viewMode=0;cfg.tabContainer.x=780;cfg.tabContainer.y=160;
    cfg.tabContainer.width=585;cfg.tabContainer.height=730;cfg.tabContainer.locked=false;
    cfg.currentCategoryId=L"first";
    CategoryConfig first;first.id=L"first";first.name=L"开发";first.layout=cfg.window;first.layout.x=470;
    for(int i=0;i<3;++i) {
        const auto path=root/("p82-file-"+std::to_string(i)+".txt");
        std::ofstream file(path);file<<"p82 original workspace file "<<i<<"\n";file.close();
        ItemConfig item;item.id=L"p82-"+std::to_wstring(i);item.path=path.wstring();item.displayName=path.filename().wstring();
        cfg.items.push_back(item);first.itemIds.push_back(item.id);
    }
    CategoryConfig second;second.id=L"second";second.name=L"文档";second.layout=cfg.window;second.layout.x=880;
    cfg.categories={first,second};Need(store.SaveAppConfig(cfg),"seed atomic config");
}
}
int RunSmokeDisplayModes(HINSTANCE instance) {
    try {
        wchar_t env[32768]{};GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",env,ARRAYSIZE(env));
        Need(env[0]!=0,"workspace isolated fixture required");const std::filesystem::path evidence=env;
        SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR",evidence.c_str());
        ConfigStore store;Seed(store,evidence);const auto initial=store.LoadAppConfig();
        MONITORINFOEXW monitor{sizeof(monitor)};
        const auto handle=MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
        Need(GetMonitorInfoW(handle,&monitor)!=FALSE,"fixture monitor");
        LayoutMonitorSnapshot topology;
        topology.id=monitor.szDevice;
        topology.workArea={monitor.rcWork.left,monitor.rcWork.top,monitor.rcWork.right,monitor.rcWork.bottom};
        topology.dpi=static_cast<int>(GetDpiForSystem());topology.primary=true;
        const auto snap=BuildLayoutSnapshot(initial,{topology},1);
        Need(snap.hasTabContainer&&SameRect(snap.tabContainer,initial.tabContainer),"layout captures tab container");
        Need(store.SaveLayoutProfile(snap),"save schema2 optional tabs");
        LayoutSnapshot loaded;Need(store.LoadLayoutProfile(loaded)&&loaded.hasTabContainer,"load optional tabs");
        Need(SameRect(loaded.tabContainer,initial.tabContainer)&&loaded.currentCategoryId==L"first","profile independent tab roundtrip");
        {
            MainWindow main(instance,[](HWND,std::wstring&){return true;});
            Need(main.Create(),"Main create");
            std::wstring error;Need(main.EnableDesktopDisplayTakeover(error),"takeover create");main.Show(SW_SHOWNOACTIVATE);
            PumpUntil([&]{return DisplayModesSmokeAccess::Ready(main);});
            DisplayModesSmokeAccess::Mode(main,0);
            PumpUntil([&]{return IsWindowVisible(main.Window())!=FALSE;});
            auto* surface=DisplayModesSmokeAccess::Surface(main);
            Need(surface->HostedWidgetCount()==1,"exact one shared active-category view");
            Need(GetAncestor(main.Window(),GA_PARENT)==GetAncestor(surface->Window(),GA_PARENT),"desktop sibling parent");
            const auto tabs=DisplayModesSmokeAccess::Active(main);
            std::cout<<"tab_expected="<<initial.tabContainer.x<<","<<initial.tabContainer.y<<","<<initial.tabContainer.width<<","<<initial.tabContainer.height<<","<<initial.tabContainer.locked<<","<<initial.tabContainer.collapsed
                <<" actual="<<tabs.x<<","<<tabs.y<<","<<tabs.width<<","<<tabs.height<<","<<tabs.locked<<","<<tabs.collapsed<<"\n";
            Need(SameRect(tabs,initial.tabContainer),"restore tab saved geometry");
            Need(DisplayModesSmokeAccess::View(main).CategoryId()==L"first","current tab identity");
            wchar_t collapseOnly[2]{};GetEnvironmentVariableW(L"LATTICE_SMOKE_COLLAPSE_MESSAGE_ONLY",collapseOnly,ARRAYSIZE(collapseOnly));
            if(collapseOnly[0]==L'1') {
                const int currentDpi=static_cast<int>(GetDpiForWindow(main.Window()));
                const RECT bounds=DisplayModesSmokeAccess::View(main).HostPixelBounds();
                const POINT press{bounds.left+MulDiv(49,currentDpi,96),bounds.top+MulDiv(16,currentDpi,96)};
                SendMessageW(surface->Window(),WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(press.x,press.y));
                const bool expanded=!DisplayModesSmokeAccess::Active(main).collapsed;
                const bool moving=DisplayModesSmokeAccess::ContainerMoving(main);
                SendMessageW(surface->Window(),WM_CANCELMODE,0,0);
                std::cout<<"message_only_container_double expanded="<<expanded<<" moving="<<moving<<std::endl;
                Need(expanded&&moving,"message-only header alternate press must move without collapse");
                return 0;
            }
            const int dpi=static_cast<int>(GetDpiForWindow(main.Window()));
            const RECT content=DisplayModesSmokeAccess::Grid(main),tab0=DisplayModesSmokeAccess::Tab(main,0);
            Need(tab0.top==MulDiv(2,dpi,96)&&tab0.bottom==MulDiv(30,dpi,96)&&content.top==MulDiv(32,dpi,96),"header tabs directly precede content");
            const RECT viewport=main.Window()?DisplayModesSmokeAccess::Viewport(main):RECT{};
            RECT clientHeader{};GetClientRect(main.Window(),&clientHeader);
            Need(viewport.left==MulDiv(52,dpi,96)&&viewport.right==clientHeader.right-MulDiv(101,dpi,96),"top tabs reserve six shared buttons");
            Need(DisplayModesSmokeAccess::Title(main).empty(),"no duplicate category title");
            Need(!GetDlgItem(main.Window(),4001)&&!GetDlgItem(main.Window(),4002),"search and scope controls removed");
            POINT origin{content.left+2,content.top+2};ClientToScreen(main.Window(),&origin);
            const HWND atContent=WindowFromPoint(origin);
            if(atContent!=surface->Window()) {
                DWORD hitProcess=0;GetWindowThreadProcessId(atContent,&hitProcess);
                wchar_t hitClass[128]{};GetClassNameW(atContent,hitClass,ARRAYSIZE(hitClass));
                std::wcout<<L"content_hit_owner own="<<(hitProcess==GetCurrentProcessId())<<L" class="<<hitClass<<L"\n";
                RECT mainRect{},surfaceRect{};GetWindowRect(main.Window(),&mainRect);GetWindowRect(surface->Window(),&surfaceRect);
                HRGN mainRegion=CreateRectRgn(0,0,0,0),surfaceRegion=CreateRectRgn(0,0,0,0);
                const int mainKind=GetWindowRgn(main.Window(),mainRegion),surfaceKind=GetWindowRgn(surface->Window(),surfaceRegion);
                std::cout<<"content_hit selfMain="<<(atContent==main.Window())<<" main_region="<<mainKind<<","<<PtInRegion(mainRegion,origin.x-mainRect.left,origin.y-mainRect.top)
                    <<" surface_region="<<surfaceKind<<","<<PtInRegion(surfaceRegion,origin.x-surfaceRect.left,origin.y-surfaceRect.top)<<" point="<<origin.x<<","<<origin.y<<"\\n";
                DeleteObject(mainRegion);DeleteObject(surfaceRegion);
            }
            Need(atContent==surface->Window(),"content hole routes real pointer to shared Surface");
            RedrawWindow(main.Window(),nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_INVALIDATE);
            RedrawWindow(surface->Window(),nullptr,nullptr,RDW_UPDATENOW|RDW_INVALIDATE);
            POINT headerPoint{MulDiv(49,dpi,96),MulDiv(16,dpi,96)};ClientToScreen(main.Window(),&headerPoint);
            const HWND headerHit=WindowFromPoint(headerPoint);
            std::cout<<"chrome_header_hit="<<(headerHit==surface->Window())<<" visible="<<IsWindowVisible(main.Window())<<"\\n";
            Need(headerHit==surface->Window(),"real header pointer reaches chrome");
            DwmFlush();
            Need(SaveSmokeWindowScreen(main.Window(),(evidence/L"tabs-normal-144.bmp").wstring()),"tab screen frame");
            const auto gridOracleSaved=DisplayModesSmokeAccess::GridOracle(main,tabs);
            RedrawWindow(surface->Window(),nullptr,nullptr,RDW_UPDATENOW|RDW_INVALIDATE);DwmFlush();
            Need(SaveSmokeWindowScreen(surface->Window(),(evidence/L"grid-style-oracle-144.bmp").wstring()),"grid style screen Oracle");
            { std::ofstream roi(evidence/L"style-roi.txt"); RECT host{};GetWindowRect(surface->Window(),&host);
              roi<<tabs.x-host.left<<" "<<tabs.y-host.top<<" "<<MulDiv(48,dpi,96)<<" "<<MulDiv(28,dpi,96)<<"\n";
              roi<<tabs.x-host.left+tabs.width-MulDiv(97,dpi,96)<<" "<<tabs.y-host.top<<" "<<MulDiv(97,dpi,96)<<" "<<MulDiv(28,dpi,96)<<"\n"; }
            DisplayModesSmokeAccess::EndOracle(main,gridOracleSaved);
            const POINT first=DisplayModesSmokeAccess::ItemPoint(main,0),second=DisplayModesSmokeAccess::ItemPoint(main,1);
            Click(surface->Window(),first);Click(surface->Window(),second,true);
            Need(DisplayModesSmokeAccess::View(main).SelectedItemIds().size()==2,"real content click control selection two");
            DisplayModesSmokeAccess::Refresh(main);
            Need(DisplayModesSmokeAccess::View(main).SelectedItemIds().size()==2,"content republish retains both selection");
            Click(surface->Window(),first);
            CancelledKey(surface->Window(),VK_F2,1);Need(renameSeen,"F2 actual rename dialog and cancel");
            CancelledKey(surface->Window(),VK_APPS,2);Need(menuSeen,"actual Shell selection menu and cancel");
            Need(std::filesystem::exists(evidence/"p82-file-0.txt"),"rename/menu cancel original path preserved");
            const auto hb=DisplayModesSmokeAccess::View(main).HostPixelBounds();
            {
                const auto beforeHover=DisplayModesSmokeAccess::Active(main);
                RECT beforeRect{};GetWindowRect(main.Window(),&beforeRect);
                for(const POINT p:{first,second,POINT{hb.left+MulDiv(49,dpi,96),hb.top+MulDiv(16,dpi,96)},POINT{hb.right-4,hb.bottom-4}}) {
                    POINT screen=p;ClientToScreen(surface->Window(),&screen);
                    MouseInput(screen,0);PumpForInput(20);
                }
                SendMessageW(surface->Window(),WM_MOUSELEAVE,0,0);
                SendMessageW(main.Window(),WM_MOUSELEAVE,0,0);
                RECT afterRect{};GetWindowRect(main.Window(),&afterRect);
                Need(!DisplayModesSmokeAccess::Active(main).collapsed&&SameRect(beforeHover,DisplayModesSmokeAccess::Active(main))&&EqualRect(&beforeRect,&afterRect),"hover and leave preserve expanded container and geometry");
                const POINT start{hb.left+MulDiv(49,dpi,96),hb.top+MulDiv(16,dpi,96)};
                // Message-level alternate header press; real no-button pointer
                // movement above is separately validated and never relabeled.
                SendMessageW(surface->Window(),WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(start.x,start.y));
                const bool doubleExpanded=!DisplayModesSmokeAccess::Active(main).collapsed;
                const bool doubleCaptured=GetCapture()==surface->Window();
                DisplayModesSmokeAccess::ContinuePointer(main,POINT{start.x+60,start.y+60});
                SendMessageW(surface->Window(),WM_KEYDOWN,VK_ESCAPE,0);
                SendMessageW(surface->Window(),WM_LBUTTONUP,0,MAKELPARAM(start.x+60,start.y+60));
                GetWindowRect(main.Window(),&afterRect);
                std::cout<<"controlled_header_double expanded="<<doubleExpanded<<" captured="<<doubleCaptured<<" final_expanded="<<!DisplayModesSmokeAccess::Active(main).collapsed<<std::endl;
                Need(doubleExpanded&&doubleCaptured&&!DisplayModesSmokeAccess::Active(main).collapsed&&EqualRect(&beforeRect,&afterRect)&&GetCapture()!=surface->Window(),"alternate double-click header drag never collapses and Esc restores geometry");
            }
            const RECT collapse{hb.left,hb.top,hb.left+MulDiv(24,dpi,96),hb.top+MulDiv(32,dpi,96)};
            const POINT cp{(collapse.left+collapse.right)/2,(collapse.top+collapse.bottom)/2};
            Click(surface->Window(),cp);Need(DisplayModesSmokeAccess::Active(main).collapsed&&surface->HostedWidgetCount()==1&&DisplayModesSmokeAccess::View(main).ItemAt(0)==nullptr,"container collapse retains only shared title");
            Click(surface->Window(),cp);Need(!DisplayModesSmokeAccess::Active(main).collapsed&&surface->HostedWidgetCount()==1,"container expand restores shared content");
            const RECT lock{collapse.right,collapse.top,collapse.right+MulDiv(24,dpi,96),collapse.bottom};const POINT lp{(lock.left+lock.right)/2,(lock.top+lock.bottom)/2};
            Click(surface->Window(),lp);Need(DisplayModesSmokeAccess::Active(main).locked,"container lock");Click(surface->Window(),lp);
            Need(!DisplayModesSmokeAccess::Active(main).locked,"container unlock");
            const auto savedBounds=DisplayModesSmokeAccess::View(main).HostPixelBounds();
            const POINT moveStart{savedBounds.left+MulDiv(49,dpi,96),savedBounds.top+MulDiv(16,dpi,96)};
            const POINT moveEnd{moveStart.x+60,moveStart.y+60};
            SendMessageW(surface->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(moveStart.x,moveStart.y));
            DisplayModesSmokeAccess::ContinuePointer(main,moveEnd);
            RECT preview{};GetWindowRect(main.Window(),&preview);
            Need(preview.left==tabs.x+60&&preview.top==tabs.y+60,"shared move preview follows chrome");
            RECT hostScreen{};GetWindowRect(surface->Window(),&hostScreen);
            const RECT live=DisplayModesSmokeAccess::View(main).HostPixelBounds();
            RECT liveScreen=live;OffsetRect(&liveScreen,hostScreen.left,hostScreen.top);
            Need(EqualRect(&liveScreen,&preview)!=FALSE,"live full view and chrome same bounds before mouse up");
            HRGN liveRegion=CreateRectRgn(0,0,0,0);RECT regionBox{};
            Need(liveRegion&&GetWindowRgn(surface->Window(),liveRegion)!=ERROR,"live full region exists");
            GetRgnBox(liveRegion,&regionBox);DeleteObject(liveRegion);
            Need(EqualRect(&regionBox,&live)!=FALSE,"live region follows all container edges before mouse up");
            POINT liveContent{preview.right-12,preview.bottom-12};
            Need(WindowFromPoint(liveContent)==surface->Window(),"live bottom content receives actual hit before mouse up");
            DwmFlush();Need(SaveSmokeWindowScreen(main.Window(),(evidence/L"tabs-held-drag-144.bmp").wstring()),"whole live container drag screenshot before mouse up");
            std::cout<<"live_held_full_view_region_content=1 bounds="<<preview.left<<","<<preview.top<<","<<preview.right<<","<<preview.bottom<<std::endl;
            DisplayModesSmokeAccess::ResetPointer(main);GetWindowRect(main.Window(),&preview);
            Need(preview.left==tabs.x&&preview.top==tabs.y,"move cancel restores chrome");
            SendMessageW(surface->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(moveStart.x,moveStart.y));
            DisplayModesSmokeAccess::ContinuePointer(main,moveEnd);
            SendMessageW(surface->Window(),WM_LBUTTONUP,0,MAKELPARAM(moveEnd.x,moveEnd.y));
            Need(DisplayModesSmokeAccess::Active(main).x==tabs.x+60&&store.LoadAppConfig().tabContainer.x==tabs.x+60,"shared move commits tab-only geometry");
            const POINT backStart{moveEnd.x,moveEnd.y};
            SendMessageW(surface->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(backStart.x,backStart.y));
            DisplayModesSmokeAccess::ContinuePointer(main,moveStart);
            SendMessageW(surface->Window(),WM_LBUTTONUP,0,MAKELPARAM(moveStart.x,moveStart.y));
            Need(SameRect(DisplayModesSmokeAccess::Active(main),tabs),"shared move returns saved geometry");
            RedrawWindow(surface->Window(),nullptr,nullptr,RDW_UPDATENOW|RDW_INVALIDATE);
            RedrawWindow(main.Window(),nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_INVALIDATE);DwmFlush();
            const RECT currentTab=DisplayModesSmokeAccess::Tab(main,1);
            POINT pressedTab{(currentTab.left+currentTab.right)/2,(currentTab.top+currentTab.bottom)/2};ClientToScreen(main.Window(),&pressedTab);
            const HWND pressedReceiver=WindowFromPoint(pressedTab);
            std::cout<<"actual_tab_preflight point="<<pressedTab.x<<","<<pressedTab.y<<" main="<<(pressedReceiver==main.Window())<<" surface="<<(pressedReceiver==surface->Window())<<std::endl;
            Need(pressedReceiver==main.Window()||pressedReceiver==surface->Window(),"actual tab input own receiver preflight");
            struct ReleaseOwnedPress {POINT point;~ReleaseOwnedPress(){MouseInput(point,MOUSEEVENTF_LEFTUP);}} releaseOwnedPress{pressedTab};
            MouseInput(pressedTab,MOUSEEVENTF_LEFTDOWN);
            std::cout<<"actual_tab_stage=capture"<<std::endl;
            PumpUntil([&]{return GetCapture()==surface->Window();});
            Need(DisplayModesSmokeAccess::NoopPreviews(main)==0,"one hundred unchanged moving points produce zero geometry publications");
            const POINT pressedMoved{pressedTab.x+180,pressedTab.y+120};MouseInput(pressedMoved,MOUSEEVENTF_MOVE);
            std::cout<<"actual_tab_stage=move"<<std::endl;
            PumpUntil([&]{RECT moved{};GetWindowRect(main.Window(),&moved);return moved.left==tabs.x+180&&moved.top==tabs.y+120;});
            GetWindowRect(main.Window(),&preview);const RECT heldView=DisplayModesSmokeAccess::View(main).HostPixelBounds();
            RECT heldScreen=heldView;OffsetRect(&heldScreen,hostScreen.left,hostScreen.top);
            Need((GetAsyncKeyState(VK_LBUTTON)&0x8000)&&EqualRect(&heldScreen,&preview)&&WindowFromPoint(POINT{preview.right-12,preview.bottom-12})==surface->Window(),"actual held tab drag moves complete content");
            DwmFlush();Need(SaveSmokeWindowScreen(main.Window(),(evidence/L"tabs-real-held-drag-144.bmp").wstring()),"actual held whole container frame");
            INPUT escape[2]{};
            for (int i=0;i<2;++i) {escape[i].type=INPUT_KEYBOARD;escape[i].ki.wVk=VK_ESCAPE;escape[i].ki.dwFlags=i?KEYEVENTF_KEYUP:0;}
            Need(SendInput(2,escape,sizeof(INPUT))==2,"actual drag Esc accepted");
            PumpUntil([&]{RECT restored{};GetWindowRect(main.Window(),&restored);return restored.left==tabs.x&&restored.top==tabs.y&&GetCapture()!=surface->Window();});
            MouseInput(pressedMoved,MOUSEEVENTF_LEFTUP);
            GetWindowRect(main.Window(),&preview);Need(preview.left==tabs.x&&preview.top==tabs.y&&GetCapture()!=surface->Window(),"actual held tab drag Esc restores complete container");
            Need(DisplayModesSmokeAccess::Current(main)==L"first","drag on current tab preserves category");
            std::cout<<"actual_held_tab_drag_and_escape=1"<<std::endl;
            const auto resizeBounds=DisplayModesSmokeAccess::View(main).HostPixelBounds();
            const POINT resizeStart{resizeBounds.right-2,resizeBounds.bottom-2};
            SendMessageW(surface->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(resizeStart.x,resizeStart.y));
            DisplayModesSmokeAccess::ContinuePointer(main,POINT{resizeStart.x+45,resizeStart.y+45});
            GetWindowRect(main.Window(),&preview);
            Need(preview.right-preview.left==tabs.width+45&&preview.bottom-preview.top==tabs.height+45,"shared resize preview follows chrome");
            DisplayModesSmokeAccess::ResetPointer(main);GetWindowRect(main.Window(),&preview);
            Need(preview.right-preview.left==tabs.width&&preview.bottom-preview.top==tabs.height,"resize cancel restores chrome");
            RedrawWindow(main.Window(),nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_INVALIDATE);
            RedrawWindow(surface->Window(),nullptr,nullptr,RDW_UPDATENOW|RDW_INVALIDATE);DwmFlush();
            Need(SaveSmokeWindowScreen(main.Window(),(evidence/L"tabs-after-shell-and-layout-144.bmp").wstring()),"post Shell and layout screen frame");
            SendMessageW(surface->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(cp.x,cp.y));
            SendMessageW(surface->Window(),WM_LBUTTONUP,0,MAKELPARAM(lp.x,lp.y));
            Need(!DisplayModesSmokeAccess::Active(main).collapsed&&GetCapture()!=surface->Window(),"cross header release does not act");
            const RECT secondTab=DisplayModesSmokeAccess::Tab(main,2);
            POINT tabPoint{(secondTab.left+secondTab.right)/2,(secondTab.top+secondTab.bottom)/2},tabScreen=tabPoint;
            ClientToScreen(main.Window(),&tabScreen);
            const auto tabAt=WindowFromPoint(tabScreen);
            const auto tabHt=SendMessageW(main.Window(),WM_NCHITTEST,0,MAKELPARAM(tabScreen.x,tabScreen.y));
            std::cout<<"tab_route main="<<(tabAt==main.Window())<<" surface="<<(tabAt==surface->Window())<<" ht="<<tabHt<<"\n";
            Need((tabAt==main.Window()||tabAt==surface->Window())&&tabHt==HTCLIENT,"real tab strip own receiver");
            POINT receiverPoint=tabScreen;ScreenToClient(tabAt,&receiverPoint);
            Click(tabAt,receiverPoint);
            Need(DisplayModesSmokeAccess::View(main).CategoryId()==L"second","switch real category");
            Need(DisplayModesSmokeAccess::View(main).ItemAt(0)==nullptr,"empty category not copied");
            DisplayModesSmokeAccess::Category(main,L"first");
            const auto dragGroup=[&](size_t targetTab) {
                auto* host=DisplayModesSmokeAccess::Surface(main);
                const POINT a=DisplayModesSmokeAccess::ItemPoint(main,0),b=DisplayModesSmokeAccess::ItemPoint(main,1);
                Click(host->Window(),a);Click(host->Window(),b,true);
                Need(DisplayModesSmokeAccess::View(main).SelectedItemIds().size()==2,"select full drag group");
                const RECT target=DisplayModesSmokeAccess::Tab(main,targetTab);
                POINT end{(target.left+target.right)/2,(target.top+target.bottom)/2};ClientToScreen(main.Window(),&end);ScreenToClient(host->Window(),&end);
                SendMessageW(host->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.x,a.y));
                DisplayModesSmokeAccess::ContinuePointer(main,end);
                SendMessageW(host->Window(),WM_LBUTTONUP,0,MAKELPARAM(end.x,end.y));
                Need(DisplayModesSmokeAccess::View(main).SelectedItemIds().size()==2,"cross tab preserves two selected items");
                Need(GetCapture()!=host->Window(),"cross tab releases capture");
            };
            dragGroup(2);Need(DisplayModesSmokeAccess::View(main).CategoryId()==L"second","group delivered to target tab");
            dragGroup(1);DisplayModesSmokeAccess::Reorder(main);
            Need(DisplayModesSmokeAccess::View(main).CategoryId()==L"first","group returns to original tab");
            for(int i=0;i<3;++i)Need(std::filesystem::exists(evidence/("p82-file-"+std::to_string(i)+".txt")),"metadata drop never moves original");
            Microsoft::WRL::ComPtr<IDataObject> data;
            Need(SUCCEEDED(CreateFileDropDataObject({(evidence/L"p82-file-0.txt").wstring(),(evidence/L"p82-file-1.txt").wstring()},data.GetAddressOf())),"real two-path OLE source");
            const RECT oleTab=DisplayModesSmokeAccess::Tab(main,2);POINT olePoint{(oleTab.left+oleTab.right)/2,(oleTab.top+oleTab.bottom)/2};ClientToScreen(main.Window(),&olePoint);
            DWORD performed=DROPEFFECT_NONE;
            Need(DisplayModesSmokeAccess::DropTarget(main)&&SUCCEEDED(DropShellDataObjectOnTarget(data.Get(),DisplayModesSmokeAccess::DropTarget(main),olePoint,MK_LBUTTON,DROPEFFECT_COPY|DROPEFFECT_MOVE,&performed)),"real OLE target tab routed");
            Need(performed==DROPEFFECT_NONE,"membership OLE never reports file move");
            PumpUntil([&]{return DisplayModesSmokeAccess::CollectionsDone(main)&&DisplayModesSmokeAccess::Members(main,L"second")==2;});
            DisplayModesSmokeAccess::Category(main,L"second");dragGroup(1);DisplayModesSmokeAccess::Reorder(main);
            Modal([&]{DisplayModesSmokeAccess::Settings(main);},1);
            Need(DisplayModesSmokeAccess::Active(main).viewMode==0,"settings cancel keeps mode");
            Modal([&]{DisplayModesSmokeAccess::Settings(main);},2);
            Need(DisplayModesSmokeAccess::Active(main).viewMode==1,"settings save changes real mode");
            DisplayModesSmokeAccess::Mode(main,0);
            Modal([&]{DisplayModesSmokeAccess::CreateCategory(main);},3,L"新标签验收");
            Need(DisplayModesSmokeAccess::Count(main)==3&&DisplayModesSmokeAccess::Name(main)==L"新标签验收","one category creation updates tabs");
            Modal([&]{DisplayModesSmokeAccess::RenameCategory(main);},3,L"标签重命名验收");
            Need(DisplayModesSmokeAccess::Name(main)==L"标签重命名验收","existing category rename updates tabs");
            Modal([&]{DisplayModesSmokeAccess::DeleteCategory(main);},4);
            Need(DisplayModesSmokeAccess::Count(main)==2&&DisplayModesSmokeAccess::Current(main)==L"second","category delete picks existing neighbor");
            DisplayModesSmokeAccess::Overflow(main,true);
            DisplayModesSmokeAccess::Category(main,L"overflow-11");
            const RECT last=DisplayModesSmokeAccess::Tab(main,14);RECT client{};GetClientRect(main.Window(),&client);
            Need(last.left>=viewport.left&&last.right<=viewport.right,"restored far tab visible between buttons");
            POINT wheel{viewport.left+4,(viewport.top+viewport.bottom)/2};ClientToScreen(main.Window(),&wheel);
            for(int i=0;i<20;++i)SendMessageW(DisplayModesSmokeAccess::Surface(main)->Window(),WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(wheel.x,wheel.y));
            const RECT firstVisible=DisplayModesSmokeAccess::Tab(main,0);Need(firstVisible.left==viewport.left,"wheel reaches first overflow tab");
            for(int i=0;i<20;++i)SendMessageW(DisplayModesSmokeAccess::Surface(main)->Window(),WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),MAKELPARAM(wheel.x,wheel.y));
            const RECT addVisible=DisplayModesSmokeAccess::Tab(main,15);Need(addVisible.right<=viewport.right,"wheel reaches add tab without button overlap");
            DisplayModesSmokeAccess::Overflow(main,false);
            DisplayModesSmokeAccess::Category(main,L"first");
            DisplayModesSmokeAccess::Mode(main,1);
            Need(!IsWindowVisible(main.Window())&&surface->HostedWidgetCount()>=2,"restore multiple grids and hide chrome");
            const auto grid=store.LoadAppConfig();Need(SameRect(grid.window,initial.window),"grid geometry untouched");
            DisplayModesSmokeAccess::Mode(main,0);
            PumpUntil([&]{return IsWindowVisible(main.Window())!=FALSE;});
            Need(SameRect(DisplayModesSmokeAccess::Active(main),tabs),"mode roundtrip tab geometry");
            DisplayModesSmokeAccess::Hide(main);
            Need(!IsWindowVisible(main.Window())&&!DisplayModesSmokeAccess::Surface(main),"all hidden releases host");
            DisplayModesSmokeAccess::Hide(main);
            PumpUntil([&]{return DisplayModesSmokeAccess::Ready(main)&&IsWindowVisible(main.Window());});
            Need(DisplayModesSmokeAccess::View(main).CategoryId()==L"first","hidden restore current tab");
            DisplayModesSmokeAccess::Fault(main);
            Need(!IsWindowVisible(main.Window()),"fault retreats tab chrome immediately");
            SendMessageW(main.Window(),WM_CLOSE,0,0);
            Need(!IsWindow(main.Window()),"normal close");
        }
        Need(ConfigStore::DrainPendingWrites(5000),"writer drain");
        const auto after=store.LoadAppConfig();
        Need(after.window.viewMode==0&&after.currentCategoryId==L"first","persist mode and current tab");
        Need(after.categories[0].itemIds==initial.categories[0].itemIds,"one member/order model");
        for(size_t i=0;i<initial.categories.size();++i)Need(SameRect(after.categories[i].layout,initial.categories[i].layout),"all category grid layouts remain independent");
        Need(SameRect(after.window,initial.window)&&SameRect(after.tabContainer,initial.tabContainer),"independent layouts after exit");
        {
            MainWindow reopened(instance,[](HWND,std::wstring&){return true;});
            Need(reopened.Create(),"reopen create");
            Need(DisplayModesSmokeAccess::Active(reopened).viewMode==0&&SameRect(DisplayModesSmokeAccess::Active(reopened),after.tabContainer),"cross instance active saved tabs");
            SendMessageW(reopened.Window(),WM_CLOSE,0,0);
        }
        std::cout<<"PASS: schema2 optional tabs, real Main mode roundtrip/current category/empty/shared two-item selection and republish, hide/recreate/fault/close, independent persisted layouts and cross-instance mode\n";
        return 0;
    } catch(const std::exception& e) {
        std::cout<<"FAIL: display modes "<<e.what()<<"\n";return 402;
    }
}

int RunSmokeAccessHotkeys(HINSTANCE instance) {
    try {
        wchar_t env[32768]{};GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",env,ARRAYSIZE(env));
        Need(env[0]!=0,"workspace fixture");const std::filesystem::path evidence=env;
        SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR",evidence.c_str());
        ConfigStore store;Seed(store,evidence);auto seeded=store.LoadAppConfig();
        seeded.settings.quickHideKey=VK_F20;seeded.settings.temporaryAccessKey=VK_F22;
        seeded.settings.quickHideModifiers=seeded.settings.temporaryAccessModifiers=MOD_CONTROL|MOD_ALT;
        Need(store.SaveAppConfig(seeded),"seed isolated bindings");
        HWND holder=CreateWindowW(L"STATIC",L"Lattice owned hotkey conflict fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,instance,nullptr);
        Need(holder!=nullptr,"own conflict window");
        struct WindowCleanup {HWND h;~WindowCleanup(){if(IsWindow(h))DestroyWindow(h);}} cleanup{holder};
        {
            MainWindow main(instance,[](HWND,std::wstring&){return true;});Need(main.Create(),"Main create");
            Need(DisplayModesSmokeAccess::BindingKey(main,0)==VK_F20,"startup registration");
            std::wstring error;Need(main.EnableDesktopDisplayTakeover(error),"takeover");main.Show(SW_SHOWNOACTIVATE);
            PumpUntil([&]{return DisplayModesSmokeAccess::Ready(main);});
            for(int mode:{1,0}) {
                DisplayModesSmokeAccess::Mode(main,mode);const auto before=store.LoadAppConfig();
                DisplayModesSmokeAccess::FireHide(main);Need(!DisplayModesSmokeAccess::Surface(main),"hotkey invokes same all-hidden lifecycle");
                DisplayModesSmokeAccess::FireHide(main);PumpUntil([&]{return DisplayModesSmokeAccess::Ready(main);});
                const auto after=store.LoadAppConfig();Need(after.window.viewMode==mode&&after.currentCategoryId==before.currentCategoryId,"hotkey restore keeps mode/tab");
                Need(SameRect(before.tabContainer,after.tabContainer)&&SameRect(before.window,after.window),"hotkey restore keeps layouts");
                Need(before.categories[0].itemIds==after.categories[0].itemIds,"hide keeps member order");
            }
            auto reloaded=store.LoadAppConfig();reloaded.settings.quickHideKey=VK_F19;Need(store.SaveAppConfig(reloaded),"save changed shortcut through config channel");
            main.ReloadPersistedState();Need(DisplayModesSmokeAccess::BindingKey(main,0)==VK_F19,"existing config reload synchronizes binding");
            reloaded.settings.quickHideKey=VK_F20;Need(store.SaveAppConfig(reloaded),"restore config binding");main.ReloadPersistedState();
            const int originalId=DisplayModesSmokeAccess::BindingId(main,0);
            Need(RegisterHotKey(holder,1,MOD_CONTROL|MOD_ALT,VK_F21)!=FALSE,"hold actual occupied combination");
            auto candidate=seeded.settings;candidate.quickHideKey=VK_F21;
            Need(!DisplayModesSmokeAccess::Bind(main,candidate,error)&&!error.empty(),"actual OS conflict feedback");
            Need(DisplayModesSmokeAccess::BindingId(main,0)==originalId&&DisplayModesSmokeAccess::BindingKey(main,0)==VK_F20,"conflict preserves old binding");
            candidate=seeded.settings;candidate.temporaryAccessKey=VK_F20;
            Need(!DisplayModesSmokeAccess::Bind(main,candidate,error),"duplicate shortcuts rejected");
            Need(RegisterHotKey(holder,2,MOD_CONTROL|MOD_ALT,VK_F23)!=FALSE,"hold second-role conflict");
            candidate=seeded.settings;candidate.quickHideKey=VK_F24;candidate.temporaryAccessKey=VK_F23;
            Need(!DisplayModesSmokeAccess::Bind(main,candidate,error),"second registration failure rolls back fresh first");
            Need(RegisterHotKey(holder,3,MOD_CONTROL|MOD_ALT,VK_F24)!=FALSE,"fresh candidate released after rollback");UnregisterHotKey(holder,3);UnregisterHotKey(holder,2);
            candidate=seeded.settings;std::swap(candidate.quickHideKey,candidate.temporaryAccessKey);
            Need(DisplayModesSmokeAccess::Bind(main,candidate,error),"roles swap reuses actual registrations");
            Need(DisplayModesSmokeAccess::BindingKey(main,0)==VK_F22&&DisplayModesSmokeAccess::Bind(main,seeded.settings,error),"restore swapped roles");
            Modal([&]{DisplayModesSmokeAccess::Settings(main);},5);
            Need(modalConflictKept&&modalImage&&DisplayModesSmokeAccess::BindingKey(main,0)==VK_F20,"visible conflict keeps dialog and original binding");
            UnregisterHotKey(holder,1);
            Modal([&]{DisplayModesSmokeAccess::Settings(main);},6);
            Need(modalImage&&DisplayModesSmokeAccess::BindingKey(main,0)==VK_F24&&DisplayModesSmokeAccess::BindingId(main,1)==0,"capture/save/clear native settings controls");
            const auto saved=store.LoadAppConfig();Need(saved.settings.quickHideKey==VK_F24&&saved.settings.temporaryAccessKey==0,"persist optional shortcut fields");
            // One real registered combination reaches WM_HOTKEY; modifiers are released before waiting.
            INPUT input[6]{};const WORD keys[]={VK_CONTROL,VK_MENU,VK_F24,VK_F24,VK_MENU,VK_CONTROL};
            for(int i=0;i<6;++i){input[i].type=INPUT_KEYBOARD;input[i].ki.wVk=keys[i];input[i].ki.dwFlags=i>=3?KEYEVENTF_KEYUP:0;}
            Need(SendInput(6,input,sizeof(INPUT))==6,"registered actual key input");
            PumpUntil([&]{return !DisplayModesSmokeAccess::Surface(main);});
            Need(!store.LoadAppConfig().settings.lastVisible,"real WM_HOTKEY hidden persisted");
            SendMessageW(main.Window(),WM_CLOSE,0,0);Need(!IsWindow(main.Window()),"normal close");
        }
        Need(RegisterHotKey(holder,4,MOD_CONTROL|MOD_ALT,VK_F24)!=FALSE,"exit releases active shortcut");UnregisterHotKey(holder,4);
        Need(RegisterHotKey(holder,5,MOD_CONTROL|MOD_ALT,VK_F22)!=FALSE,"cleared second shortcut released");UnregisterHotKey(holder,5);
        std::cout<<"PASS: actual RegisterHotKey conflicts/second-role rollback/reuse/duplicates, both-mode hide restoration, native capture-clear-error screenshots, config persistence, real registered input, exit release\n";return 0;
    }catch(const std::exception& e){std::cout<<"FAIL: access hotkeys "<<e.what()<<"\n";return 403;}
}

int RunSmokeTemporaryAccess(HINSTANCE instance) {
    try {
        wchar_t env[32768]{};GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",env,ARRAYSIZE(env));
        Need(env[0]!=0,"workspace isolated temporary fixture");const std::filesystem::path evidence=env;
        SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR",evidence.c_str());
        ConfigStore store;Seed(store,evidence);auto initial=store.LoadAppConfig();
        const auto& root=evidence;PrepareAccessOpen(root,initial);
        initial.settings.quickHideKey=VK_F20;initial.settings.temporaryAccessKey=VK_F22;
        Need(store.SaveAppConfig(initial),"temporary fixture saved shortcut pair");
        HWND application=CreateWindowExW(0,L"STATIC",L"Lattice owned drop receiver",WS_POPUP|WS_BORDER|SS_NOTIFY,
            400,100,1800,1000,nullptr,nullptr,instance,nullptr);Need(application!=nullptr,"owned application receiver");
        POINT cursor{};GetCursorPos(&cursor);
        struct Restore {HWND h;POINT cursor;~Restore(){UnregisterShellDropTarget(h);DestroyWindow(h);SetCursorPos(cursor.x,cursor.y);}} restore{application,cursor};
        MainWindow main(instance,[](HWND,std::wstring&){return true;});Need(main.Create(),"temporary Main create");
        std::wstring error;Need(main.EnableDesktopDisplayTakeover(error),"temporary host create");main.Show(SW_SHOWNOACTIVATE);
        std::cout<<"access_wait=1"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::Ready(main);});
        for(int mode:{0,1}) {
            std::cout<<"access_mode="<<mode<<std::endl;
            ShowWindow(application,SW_HIDE);DisplayModesSmokeAccess::Mode(main,mode);
            auto* surface=DisplayModesSmokeAccess::Surface(main);Need(surface&&IsWindowVisible(surface->Window()),"initial desktop visible");
            const HWND originalParent=GetAncestor(surface->Window(),GA_PARENT),native=DisplayModesSmokeAccess::Native(main);
            const HWND nativeParent=GetAncestor(native,GA_PARENT);const UINT dpi=GetDpiForWindow(surface->Window());
            const LONG_PTR style=GetWindowLongPtrW(surface->Window(),GWL_STYLE),extended=GetWindowLongPtrW(surface->Window(),GWL_EXSTYLE);
            RECT original{};GetWindowRect(surface->Window(),&original);
            POINT point=DisplayModesSmokeAccess::AccessPoint(main,0);ClientToScreen(surface->Window(),&point);
            ShowWindow(application,SW_SHOW);SetWindowPos(application,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);SetForegroundWindow(application);DwmFlush();
            Need(WindowFromPoint(point)==application,"ordinary owned application covers desktop grid");
            DisplayModesSmokeAccess::Temporary(main);std::cout<<"access_wait=2"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
            const HWND temporaryRoot=GetAncestor(surface->Window(),GA_PARENT);
            Need(temporaryRoot!=GetDesktopWindow()&&temporaryRoot!=originalParent&&(GetWindowLongPtrW(temporaryRoot,GWL_EXSTYLE)&WS_EX_TOPMOST),"temporary own topmost");
            Need(WindowFromPoint(point)==surface->Window(),"temporary actual pointer route over ordinary application");
            Need(GetAncestor(native,GA_PARENT)==nativeParent&&GetDpiForWindow(surface->Window())==dpi,"native desktop and DPI unchanged");
            RECT promoted{};GetWindowRect(surface->Window(),&promoted);Need(EqualRect(&original,&promoted),"temporary screen geometry unchanged");
            if(mode==0)Need(GetAncestor(main.Window(),GA_PARENT)==temporaryRoot,"temporary own tab strip");
            Click(surface->Window(),DisplayModesSmokeAccess::AccessPoint(main,0));Click(surface->Window(),DisplayModesSmokeAccess::AccessPoint(main,1),true);
            Need(DisplayModesSmokeAccess::AccessView(main).SelectedItemIds().size()==2,"temporary same multi selection");
            DisplayModesSmokeAccess::Mode(main,mode);Need(DisplayModesSmokeAccess::TemporaryActive(main),"unchanged mode is idempotent");
            RECT tabActual{};GetWindowRect(main.Window(),&tabActual);const auto tabExpected=DisplayModesSmokeAccess::Active(main);
            std::cout<<"tab_geometry actual="<<tabActual.left<<","<<tabActual.top<<","<<tabActual.right<<","<<tabActual.bottom<<" expected="<<tabExpected.x<<","<<tabExpected.y<<","<<tabExpected.width<<","<<tabExpected.height<<std::endl;
            PumpForInput(100);RedrawWindow(surface->Window(),nullptr,nullptr,RDW_UPDATENOW|RDW_ALLCHILDREN|RDW_INVALIDATE);DwmFlush();
            Need(SaveSmokeWindowScreen(surface->Window(),(root/(mode?L"temporary-grids.bmp":L"temporary-tabs.bmp")).wstring()),"actual temporary visual frame");
            const size_t openedBefore=AccessOpenCount(root);POINT open=DisplayModesSmokeAccess::AccessPoint(main,2);ClientToScreen(surface->Window(),&open);
            Need(WindowFromPoint(open)==surface->Window(),"temporary controlled open pointer route");
            if(!SetCursorPos(open.x,open.y)){std::cout<<"pointer_permission error="<<GetLastError()<<std::endl;throw std::runtime_error("actual desktop pointer permission preflight");}PumpForInput(40);
            Need(GetCapture()==nullptr&&WindowFromPoint(open)==surface->Window(),"actual input capture and pointer preflight");
            INPUT doubleInput[4]{};
            for(int click=0;click<4;++click) {
                doubleInput[click].type=INPUT_MOUSE;
                doubleInput[click].mi.dx=MulDiv(open.x-GetSystemMetrics(SM_XVIRTUALSCREEN),65535,GetSystemMetrics(SM_CXVIRTUALSCREEN)-1);
                doubleInput[click].mi.dy=MulDiv(open.y-GetSystemMetrics(SM_YVIRTUALSCREEN),65535,GetSystemMetrics(SM_CYVIRTUALSCREEN)-1);
                doubleInput[click].mi.dwFlags=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|MOUSEEVENTF_MOVE|(click%2?MOUSEEVENTF_LEFTUP:MOUSEEVENTF_LEFTDOWN);
            }
            Need(SendInput(4,doubleInput,sizeof(INPUT))==4,"complete actual double click input batch");PumpForInput(100);
            DisplayModesSmokeAccess::OpenTrace(main,AccessOpenCount(root));
            PumpUntil([&]{return AccessOpenCount(root)>openedBefore;});Need(AccessOpenCount(root)==openedBefore+1&&DisplayModesSmokeAccess::TemporaryActive(main),"temporary real double click opens once");
            // Focus/shortcut release do not end. F2 follows the existing dialog.
            SetForegroundWindow(application);SendMessageW(surface->Window(),WM_KEYUP,VK_F10,0);
            Need(DisplayModesSmokeAccess::TemporaryActive(main),"focus and key release keep temporary layer");
            Click(surface->Window(),DisplayModesSmokeAccess::AccessPoint(main,0));
            renameSeen=false;CancelledKey(surface->Window(),VK_F2,1);Need(renameSeen&&DisplayModesSmokeAccess::TemporaryActive(main),"temporary existing F2 cancelled");
            menuSeen=false;accessMenuMain=&main;accessMenuDeferred=false;CancelledKey(surface->Window(),VK_APPS,2);accessMenuMain=nullptr;
            Need(menuSeen&&accessMenuDeferred,"menu repeated trigger deferred during Shell transaction");
            std::cout<<"access_wait=3"<<std::endl;PumpUntil([&]{return !DisplayModesSmokeAccess::TemporaryActive(main);});
            Need(GetAncestor(surface->Window(),GA_PARENT)==originalParent&&GetWindowLongPtrW(surface->Window(),GWL_STYLE)==style&&GetWindowLongPtrW(surface->Window(),GWL_EXSTYLE)==extended,"menu exit restores exact own style parent");
            DisplayModesSmokeAccess::Temporary(main);Need(DisplayModesSmokeAccess::TemporaryActive(main),"menu-after second synchronous promotion");
            SendMessageW(surface->Window(),WM_KEYDOWN,VK_ESCAPE,0);Need(!DisplayModesSmokeAccess::TemporaryActive(main),"Esc restores temporary layer");
            // Real OLE source and existing controlled receiving target; no file copy or move.
            std::vector<std::wstring> dropped;bool oleDeferred=false;
            Need(RegisterShellDropTarget(application,[&](IDataObject*,const auto& paths,POINT,DWORD,DWORD,DWORD* effect){dropped=paths;*effect=DROPEFFECT_COPY;return true;},[]{return true;},[&](ShellDropPreviewEvent event,const auto&,POINT){
                if(event==ShellDropPreviewEvent::Enter&&DisplayModesSmokeAccess::TemporaryActive(main)) {
                    DisplayModesSmokeAccess::Temporary(main);oleDeferred=DisplayModesSmokeAccess::TemporaryActive(main)&&DisplayModesSmokeAccess::EndPending(main);
                }
            }),"register existing Shell receiver");
            DisplayModesSmokeAccess::Temporary(main);std::cout<<"access_wait=5"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
            Click(surface->Window(),DisplayModesSmokeAccess::AccessPoint(main,0));Click(surface->Window(),DisplayModesSmokeAccess::AccessPoint(main,1),true);
            Need(DisplayModesSmokeAccess::AccessView(main).SelectedItemIds().size()==2,"OLE selected two originals");
            POINT source=DisplayModesSmokeAccess::AccessPoint(main,0);ClientToScreen(surface->Window(),&source);
            accessDropPoint={1900,700};accessDragStep=0;
            MouseInput(source,MOUSEEVENTF_LEFTDOWN);
            PumpUntil([&]{return GetCapture()==surface->Window();});
            const UINT_PTR timer=SetTimer(nullptr,0,140,DriveAccessDrag);
            MouseInput(POINT{source.x+20,source.y},0);
            std::cout<<"access_wait=6 route="<<reinterpret_cast<uintptr_t>(WindowFromPoint(source))<<" source="<<reinterpret_cast<uintptr_t>(surface->Window())<<std::endl;
            try {PumpUntil([&]{return dropped.size()==2;});} catch(...) {std::cout<<"ole_result starts="<<DisplayModesSmokeAccess::DragStarts(main)<<" dropped="<<dropped.size()<<" deferred="<<oleDeferred<<" selected="<<DisplayModesSmokeAccess::AccessView(main).SelectedItemIds().size()<<std::endl;throw;} KillTimer(nullptr,timer);
            std::cout<<"access_wait=7"<<std::endl;PumpUntil([&]{return !DisplayModesSmokeAccess::TemporaryActive(main);});
            Need(oleDeferred&&dropped[0]==initial.items[0].path&&dropped[1]==initial.items[1].path,"actual OLE grouped original paths and pending end");
            Need(DisplayModesSmokeAccess::AccessView(main).SelectedItemIds().size()==2,"OLE keeps original two selection");
            UnregisterShellDropTarget(application);
            DisplayModesSmokeAccess::Temporary(main);PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
            Click(surface->Window(),DisplayModesSmokeAccess::AccessPoint(main,0));Click(surface->Window(),DisplayModesSmokeAccess::AccessPoint(main,1),true);
            const auto starts=DisplayModesSmokeAccess::DragStarts(main);source=DisplayModesSmokeAccess::AccessPoint(main,0);ClientToScreen(surface->Window(),&source);
            accessDragCancel=true;accessDragStep=0;MouseInput(source,MOUSEEVENTF_LEFTDOWN);PumpUntil([&]{return GetCapture()==surface->Window();});
            const UINT_PTR cancelTimer=SetTimer(nullptr,0,140,DriveAccessDrag);MouseInput(POINT{source.x+20,source.y},0);
            PumpUntil([&]{return !DisplayModesSmokeAccess::TemporaryActive(main);});PumpForInput(200);KillTimer(nullptr,cancelTimer);accessDragCancel=false;
            Need(DisplayModesSmokeAccess::DragStarts(main)==starts+1&&DisplayModesSmokeAccess::AccessView(main).SelectedItemIds().size()==2,"real OLE Esc cancellation restores layer and selection");
            // Hidden trigger is transient even if metadata is saved.
            ShowWindow(application,SW_HIDE);DisplayModesSmokeAccess::Hide(main);Need(!store.LoadAppConfig().settings.lastVisible,"hidden state baseline");
            DisplayModesSmokeAccess::Temporary(main);std::cout<<"access_wait=8"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
            DisplayModesSmokeAccess::SaveMetadata(main);Need(!store.LoadAppConfig().settings.lastVisible,"temporary hidden display does not persist visible");
            DisplayModesSmokeAccess::Temporary(main);Need(!DisplayModesSmokeAccess::Surface(main)&&!IsWindowVisible(main.Window()),"repeat restores preexisting hidden state");
            DisplayModesSmokeAccess::Temporary(main);PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});DisplayModesSmokeAccess::FireHide(main);
            Need(!DisplayModesSmokeAccess::Surface(main)&&!store.LoadAppConfig().settings.lastVisible,"quick hide from initially hidden temporary stays hidden");
            DisplayModesSmokeAccess::Hide(main);std::cout<<"access_wait=9"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::Ready(main);});
            DisplayModesSmokeAccess::Temporary(main);std::cout<<"access_wait=10"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
            DisplayModesSmokeAccess::FireHide(main);Need(!DisplayModesSmokeAccess::Surface(main)&&!store.LoadAppConfig().settings.lastVisible,"quick hide reliably ends temporary");
            DisplayModesSmokeAccess::Hide(main);std::cout<<"access_wait=11"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::Ready(main);});
        }
        DisplayModesSmokeAccess::Temporary(main);PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
        Microsoft::WRL::ComPtr<IDataObject> incoming;Need(SUCCEEDED(CreateFileDropDataObject({initial.items[0].path},incoming.GetAddressOf())),"incoming existing data object");
        auto* target=DisplayModesSmokeAccess::DropTarget(main);target->AddRef();POINT incomingPoint=DisplayModesSmokeAccess::AccessPoint(main,0);ClientToScreen(DisplayModesSmokeAccess::Surface(main)->Window(),&incomingPoint);
        POINTL incomingScreen{incomingPoint.x,incomingPoint.y};DWORD incomingEffect=DROPEFFECT_COPY;
        Need(SUCCEEDED(target->DragEnter(incoming.Get(),MK_LBUTTON,incomingScreen,&incomingEffect)),"incoming OLE enter");
        DisplayModesSmokeAccess::Temporary(main);Need(DisplayModesSmokeAccess::TemporaryActive(main)&&DisplayModesSmokeAccess::EndPending(main),"incoming OLE defers repeated trigger");
        const HWND recoverySource=DisplayModesSmokeAccess::Surface(main)->Window();
        SendMessageW(main.Window(),RegisterWindowMessageW(L"TaskbarCreated"),0,0);Need(!DisplayModesSmokeAccess::TemporaryActive(main),"Explorer recovery withdraws temporary layer immediately");
        target->DragLeave();target->Release();PumpUntil([&]{return !IsWindow(recoverySource)&&DisplayModesSmokeAccess::Ready(main);});
        DisplayModesSmokeAccess::Temporary(main);std::cout<<"access_wait=12"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
        DisplayModesSmokeAccess::Mode(main,DisplayModesSmokeAccess::Active(main).viewMode==0?1:0);Need(!DisplayModesSmokeAccess::TemporaryActive(main),"mode change ends temporary");
        DisplayModesSmokeAccess::Temporary(main);std::cout<<"access_wait=13"<<std::endl;PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
        HWND surfaceWindow=DisplayModesSmokeAccess::Surface(main)->Window();DisplayModesSmokeAccess::Fault(main);
        Need(!DisplayModesSmokeAccess::TemporaryActive(main)&&!IsWindowVisible(surfaceWindow)&&!(GetWindowLongPtrW(surfaceWindow,GWL_EXSTYLE)&WS_EX_TOPMOST)&&!IsWindowVisible(main.Window()),"fault withdraws both temporary layers");
        DisplayModesSmokeAccess::Surface(main)->Show();PumpUntil([&]{return IsWindowVisible(DisplayModesSmokeAccess::Surface(main)->Window())!=FALSE;});
        DisplayModesSmokeAccess::Temporary(main);PumpUntil([&]{return DisplayModesSmokeAccess::TemporaryActive(main);});
        const HWND finalRoot=GetAncestor(DisplayModesSmokeAccess::Surface(main)->Window(),GA_PARENT);
        SendMessageW(main.Window(),WM_CLOSE,0,0);Need(!IsWindow(main.Window())&&!IsWindow(finalRoot),"temporary normal exit destroys transient root");
        const auto final=store.LoadAppConfig();Need(SameRect(final.window,initial.window)&&SameRect(final.tabContainer,initial.tabContainer),"temporary independent layouts unchanged");
        for(const auto& item:initial.items)Need(std::filesystem::exists(item.path),"workspace originals unchanged");
        std::cout<<"PASS: both-mode actual foreground route, own parent/style/rect/DPI restoration, native desktop unchanged, multi-selection/F2/Shell menu pending end, real two-item OLE to owned receiver, hidden persistence, Esc/repeat/hide/mode/fault/exit"<<std::endl;return 0;
    }catch(const std::exception& e){accessMenuMain=nullptr;std::cout<<"FAIL: temporary access "<<e.what()<<std::endl;return 404;}
}
