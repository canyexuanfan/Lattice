#include "ui/MainWindow.h"

#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <iterator>
#include <unordered_set>

#include "desktop/DesktopScanner.h"
#include "desktop/DesktopLayout.h"
#include "desktop/CategoryStorageManager.h"
#include "app/resource.h"
#include "ui/InputDialog.h"
#include "ui/DragGhostWindow.h"
#include "ui/MessageDialog.h"
#include "util/PathUtil.h"
#include "shell/ShellDragDrop.h"
#include "util/StringUtil.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"Lattice.MainWindow";
constexpr int kTitleHeight = 38;
constexpr int kTabsTop = 40;
constexpr int kTabsHeight = 34;
constexpr int kResizeGrip = 18;
constexpr int kWheelScrollPixels = 84;
constexpr int kSideTabsWidth = 136;
constexpr int kTileHeaderHeight = 42;
constexpr int kTileRowGap = 4;
constexpr int kTileExpandedHeight = 196;
constexpr int kTilePanelWidth = 380;
constexpr int kTilePanelHeight = 820;
constexpr int kTilePanelMinWidth = 320;
constexpr int kTilePanelMaxWidth = 440;
constexpr int kNewCategoryCommand = 2001;
constexpr int kRenameCategoryCommand = 2002;
constexpr int kDeleteCategoryCommand = 2003;
constexpr int kExitCommand = 2004;
constexpr int kToggleCollapseCommand = 2005;
constexpr int kToggleLockCommand = 2006;
constexpr int kIconSmallCommand = 2007;
constexpr int kIconMediumCommand = 2008;
constexpr int kIconLargeCommand = 2009;
constexpr int kImportDesktopCommand = 2010;
constexpr int kRefreshDesktopCommand = 2011;
constexpr int kOpenWidgetCommand = 2012;
constexpr int kMoveCategoryUpCommand = 2013;
constexpr int kMoveCategoryDownCommand = 2014;
constexpr int kCategoryColorBlueCommand = 2025;
constexpr int kCategoryColorMintCommand = 2026;
constexpr int kCategoryColorAmberCommand = 2027;
constexpr int kCategoryColorVioletCommand = 2028;
constexpr int kCategoryIconFolderCommand = 2029;
constexpr int kExportConfigCommand = 2030;
constexpr int kImportConfigCommand = 2031;
constexpr int kCategoryIconAppsCommand = 2032;
constexpr int kCategoryIconWorkCommand = 2033;
constexpr int kCategoryIconDocsCommand = 2034;
constexpr int kCategoryIconMediaCommand = 2035;
constexpr int kExportCategoryCommand = 2036;
constexpr int kImportCategoryCommand = 2037;
constexpr int kViewTabsCommand = 2038;
constexpr int kViewTileCommand = 2039;
constexpr int kTabTopCommand = 2063;
constexpr int kTabBottomCommand = 2064;
constexpr int kTabLeftCommand = 2065;
constexpr int kTabRightCommand = 2066;
constexpr int kSaveLayoutProfileCommand = 2060;
constexpr int kRestoreLayoutProfileCommand = 2061;
constexpr int kResetLayoutCommand = 2062;
constexpr int kDensityCompactCommand = 2040;
constexpr int kDensityStandardCommand = 2041;
constexpr int kDensitySpaciousCommand = 2042;
constexpr int kTrayToggleVisibleCommand = 2020;
constexpr int kTrayToggleLockCommand = 2021;
constexpr int kTrayRefreshCommand = 2022;
constexpr int kTrayStartupCommand = 2023;
constexpr int kTraySettingsCommand = 2024;
constexpr int kSearchEditId = 4001;
constexpr int kSearchScopeId = 4002;
constexpr UINT kDesktopChangedMessage = WM_APP + 11;
constexpr UINT kIconReadyMessage = WM_APP + 14;
const UINT kUpdateExitMessage = RegisterWindowMessageW(L"Lattice.RequestExitForUpdate.V1");
constexpr UINT kDesktopRefreshTimerId = 7001;
constexpr int kOpenItemCommand = 2101;
constexpr int kShowItemCommand = 2102;
constexpr int kRemoveItemCommand = 2103;
constexpr int kRunAsAdminCommand = 2104;
constexpr int kRenameItemCommand = 2105;
constexpr int kRefreshIconCommand = 2106;
constexpr int kMoveItemBaseCommand = 3000;
constexpr int kTileToggleCollapseCommand = 17;
constexpr int kTileOpenWidgetCommand = 18;

const wchar_t* DeskGoPaletteColor(size_t index) {
    constexpr const wchar_t* kPalette[] = {
        L"#2D8CFF",
        L"#35B47E",
        L"#7C5CFC",
        L"#F3A847",
        L"#E46A8A",
        L"#2BB7B0",
        L"#6CA8FF",
        L"#8B9AA6",
        L"#38C6B4",
    };
    return kPalette[index % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

std::wstring EffectiveCategoryColor(const std::wstring& color, size_t index) {
    if (color.empty() || color == L"#2D8CFF" || color == L"#2d8cff") {
        return DeskGoPaletteColor(index);
    }
    return color;
}

bool SameDesktopItems(
    const std::vector<DesktopItem>& left,
    const std::vector<DesktopItem>& right) {
    return left.size() == right.size() &&
           std::equal(
               left.begin(),
               left.end(),
               right.begin(),
               [](const DesktopItem& first, const DesktopItem& second) {
                   return first.id == second.id &&
                          first.displayName == second.displayName &&
                          first.path == second.path &&
                          first.targetPath == second.targetPath &&
                          first.arguments == second.arguments &&
                          first.workingDirectory == second.workingDirectory &&
                          first.kind == second.kind &&
                          first.missing == second.missing;
               });
}

RECT RectFromConfig(const WindowConfig& config) {
    return RECT{config.x, config.y, config.x + config.width, config.y + config.height};
}

D2D1::ColorF CategoryAccentColor(const std::wstring& value, FLOAT alpha) {
    if (value.size() == 7 && value.front() == L'#') {
        try {
            const unsigned long rgb = std::stoul(value.substr(1), nullptr, 16);
            return D2D1::ColorF(
                static_cast<FLOAT>((rgb >> 16) & 0xFF) / 255.0f,
                static_cast<FLOAT>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<FLOAT>(rgb & 0xFF) / 255.0f,
                alpha);
        } catch (...) {
        }
    }
    return D2D1::ColorF(0x26708F, alpha);
}

bool UseLightTheme(int theme) {
    if (theme == 1) {
        return true;
    }
    if (theme == 2) {
        const COLORREF systemColor = GetSysColor(COLOR_WINDOW);
        const int luminance = (static_cast<int>(GetRValue(systemColor)) * 299 +
                               static_cast<int>(GetGValue(systemColor)) * 587 +
                               static_cast<int>(GetBValue(systemColor)) * 114) / 1000;
        return luminance >= 160;
    }
    return false;
}

}  // namespace

MainWindow::MainWindow(HINSTANCE instance)
    : instance_(instance) {
    LoadOrganizerConfig();
    windowConfig_ = organizerConfig_.window;
    if (windowConfig_.collapsed) {
        windowConfig_.height = kTitleHeight + kTabsHeight + 12;
    }
}

MainWindow::~MainWindow() {
    for (auto& widget : widgetWindows_) {
        if (widget != nullptr) {
            widget->Close();
        }
    }
    widgetWindows_.clear();
    desktopWatcher_.Stop();
    trayIcon_.Remove();
}

void MainWindow::FinishPendingDesktopPlacements() {
    HandleDesktopPlacementEvents(false);
    const bool drained = desktopPlacementCoordinator_.DrainFor(5000);
    HandleDesktopPlacementEvents(false);
    if (!drained) {
        OutputDebugStringW(
            L"Lattice timed out finishing an Explorer desktop placement before desktop restoration; cancelling it now.\n");
    }
    if (!desktopPlacementCoordinator_.CancelAndStopFor(5000)) {
        OutputDebugStringW(
            L"Lattice is still cancelling a blocked Explorer desktop placement before desktop restoration.\n");
        desktopPlacementCoordinator_.CancelAndStopFor(INFINITE);
    }
    HandleDesktopPlacementEvents(false);
    DragGhostWindow::Instance().End();
}

bool MainWindow::QueueDesktopPlacement(const DesktopPlacementRequest& request) {
    DesktopPlacementRequest normalizedRequest = request;
    if (normalizedRequest.sourceWindow == nullptr ||
        IsWindow(normalizedRequest.sourceWindow) == FALSE) {
        normalizedRequest.sourceWindow =
            hwnd_ != nullptr && IsWindow(hwnd_) != FALSE ? hwnd_ : nullptr;
    }
    return desktopPlacementCoordinator_.PlaceAtScreenAsync(normalizedRequest) != 0;
}

void MainWindow::HandleDesktopPlacementEvents(bool allowDialogs) {
    for (DesktopPlacementCoordinator::Event& event :
         desktopPlacementCoordinator_.TakeEvents()) {
        if (event.stage == DesktopPlacementCoordinator::EventStage::Visible) {
            continue;
        }
        DragGhostWindow::Instance().EndIfGeneration(event.dragGhostGeneration);
        if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) {
            PostMessageW(hwnd_, kOrganizerConfigChangedMessage, 0, 0);
        }
        if (event.succeeded) {
            SaveDesktopPlacement(
                event.path,
                event.finalPoint,
                event.showError,
                event.sourceWindow,
                allowDialogs);
            continue;
        }

        const std::wstring errorMessage = event.errorMessage.empty()
            ? L"文件已安全归还桌面，但 Explorer 未能把图标放到鼠标释放位置。"
            : event.errorMessage;
        if (event.showError && allowDialogs) {
            HWND dialogOwner =
                event.sourceWindow != nullptr && IsWindow(event.sourceWindow) != FALSE
                    ? event.sourceWindow
                    : (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE ? hwnd_ : nullptr);
            MessageDialog::Show(
                instance_,
                dialogOwner,
                errorMessage.c_str(),
                L"桌面坐标恢复",
                MB_OK | MB_ICONWARNING);
        } else {
            const std::wstring diagnostic =
                L"Lattice background desktop placement failed for " +
                event.path + L": " + errorMessage + L"\n";
            OutputDebugStringW(diagnostic.c_str());
        }
    }
}

void MainWindow::FlushDeferredRefresh() {
    if (!refreshPending_) {
        return;
    }
    refreshPending_ = false;
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) {
        PostMessageW(hwnd_, kOrganizerConfigChangedMessage, 0, 0);
    }
}

bool MainWindow::SaveDesktopPlacement(
    const std::wstring& path,
    POINT point,
    bool showError,
    HWND sourceWindow,
    bool allowDialogs) {
    AppConfig updatedConfig = configStore_.LoadAppConfig();
    auto savedPosition = std::find_if(
        updatedConfig.desktopLayout.begin(),
        updatedConfig.desktopLayout.end(),
        [&](const DesktopPlacementConfig& value) {
            return CompareStringOrdinal(
                       value.path.c_str(),
                       -1,
                       path.c_str(),
                       -1,
                       TRUE) == CSTR_EQUAL;
        });
    if (savedPosition == updatedConfig.desktopLayout.end()) {
        updatedConfig.desktopLayout.push_back(
            DesktopPlacementConfig{path, point.x, point.y});
    } else {
        savedPosition->x = point.x;
        savedPosition->y = point.y;
    }
    if (configStore_.SaveAppConfig(updatedConfig)) {
        return true;
    }

    constexpr wchar_t kSaveError[] =
        L"图标已放到鼠标释放位置，但无法把新位置写入桌面布局快照。";
    if (showError && allowDialogs) {
        HWND dialogOwner =
            sourceWindow != nullptr && IsWindow(sourceWindow) != FALSE
                ? sourceWindow
                : (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE ? hwnd_ : nullptr);
        MessageDialog::Show(
            instance_,
            dialogOwner,
            kSaveError,
            L"桌面坐标保存",
            MB_OK | MB_ICONWARNING);
    } else {
        const std::wstring diagnostic =
            L"Lattice could not persist background desktop placement for " +
            path + L"\n";
        OutputDebugStringW(diagnostic.c_str());
    }
    return false;
}

bool MainWindow::Create() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = MainWindow::WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_SHARED));
    static HBRUSH backgroundBrush = CreateSolidBrush(RGB(7, 23, 32));
    windowClass.hbrBackground = backgroundBrush;
    windowClass.lpszClassName = kWindowClassName;

    RegisterClassExW(&windowClass);

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kWindowClassName,
        L"Lattice",
        WS_POPUP | WS_THICKFRAME,
        windowConfig_.x,
        windowConfig_.y,
        windowConfig_.width,
        windowConfig_.height,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ != nullptr) {
        EnsureWindowVisible();
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
    }
    return hwnd_ != nullptr;
}

void MainWindow::Show(int showCommand) {
    windowConfig_.viewMode = 1;
    organizerConfig_.window.viewMode = 1;
    ShowWindow(hwnd_, SW_HIDE);
    if (showCommand == SW_HIDE) {
        for (auto& widget : widgetWindows_) {
            if (widget != nullptr && widget->IsOpen()) {
                widget->SetVisible(false);
            }
        }
        return;
    }
    OpenAllCategoryWidgets();
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = reinterpret_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window != nullptr) {
        return window->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (kUpdateExitMessage != 0 && message == kUpdateExitMessage) {
        updateExitRequested_ = true;
        DestroyWindow(hwnd_);
        return 0;
    }
    switch (message) {
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_CREATE:
            SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(windowConfig_.opacity), LWA_ALPHA);
            DragAcceptFiles(hwnd_, TRUE);
            d2d_.Initialize(hwnd_);
            searchEdit_ = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                150,
                7,
                260,
                24,
                hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchEditId)),
                instance_,
                nullptr);
            if (searchEdit_ != nullptr) {
                SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索名称、文件名或路径…"));
                SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            }
            searchScopeCombo_ = CreateWindowW(
                L"COMBOBOX",
                L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                420,
                7,
                112,
                140,
                hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchScopeId)),
                instance_,
                nullptr);
            if (searchScopeCombo_ != nullptr) {
                SendMessageW(searchScopeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"当前分类"));
                SendMessageW(searchScopeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"全部分类"));
                SendMessageW(searchScopeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未分类"));
                SendMessageW(searchScopeCombo_, CB_SETCURSEL, searchScope_, 0);
                SendMessageW(searchScopeCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            }
            iconCache_.SetInvalidateCallback([hwnd = hwnd_]() {
                if (hwnd != nullptr && IsWindow(hwnd) != FALSE) {
                    PostMessageW(hwnd, kIconReadyMessage, 0, 0);
                }
            });
            LoadDesktopItems();
            trayIcon_.Initialize(hwnd_, instance_);
            desktopWatcher_.Start([hwnd = hwnd_]() {
                if (hwnd != nullptr) {
                    PostMessageW(hwnd, kDesktopChangedMessage, 0, 0);
                }
            });
            desktopPlacementCoordinator_.AttachNotificationWindow(hwnd_);
            LayoutSearchEdit();
            return 0;

        case WM_SIZE:
            d2d_.Resize(LOWORD(lParam), HIWORD(lParam));
            iconGrid_.SetBounds(GridBounds());
            RefreshTileViews();
            LayoutSearchEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested != nullptr) {
                SetWindowPos(
                    hwnd_,
                    nullptr,
                    suggested->left,
                    suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            windowConfig_.dpi = HIWORD(wParam);
            iconGrid_.SetBounds(GridBounds());
            RefreshTileViews();
            LayoutSearchEdit();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_DISPLAYCHANGE:
            if (windowConfig_.viewMode == 1) {
                DockTileWindowToWorkArea(false);
            }
            EnsureWindowVisible();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
            minMax->ptMinTrackSize.x = 360;
            minMax->ptMinTrackSize.y = 260;
            return 0;
        }

        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            RECT client{};
            GetClientRect(hwnd_, &client);
            if (!windowConfig_.locked && !windowConfig_.collapsed &&
                point.x >= client.right - kResizeGrip && point.y >= client.bottom - kResizeGrip) {
                return HTBOTTOMRIGHT;
            }
            if (!windowConfig_.locked && point.y >= 0 && point.y < kTitleHeight) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_LBUTTONDBLCLK: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int tabIndex = HitTestTab(point);
            if (tabIndex >= 0) {
                return 0;
            }
            if (windowConfig_.viewMode == 1) {
                size_t tileHeaderIndex = 0;
                if (HitTestTileHeader(point, tileHeaderIndex)) {
                    const std::wstring categoryId = tileViews_[tileHeaderIndex].categoryId;
                    const bool collapsed = !tileViews_[tileHeaderIndex].collapsed;
                    if (Category* category = FindCategory(categoryId); category != nullptr) {
                        category->tileCollapsed = collapsed;
                        SaveOrganizerConfig();
                    } else {
                        tileCollapsed_[categoryId] = collapsed;
                    }
                    RefreshTileViews();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                size_t tileIndex = 0;
                int tileIconIndex = -1;
                if (HitTestTileIcon(point, tileIndex, tileIconIndex)) {
                    const DesktopItem* item = tileViews_[tileIndex].grid.ItemAt(static_cast<size_t>(tileIconIndex));
                    if (item != nullptr && !organizerConfig_.settings.singleClickOpen) {
                        launcher_.OpenPath(item->path);
                    }
                }
                return 0;
            }
            const int index = iconGrid_.HitTest(point);
            if (index >= 0) {
                const DesktopItem* item = iconGrid_.ItemAt(static_cast<size_t>(index));
                if (item != nullptr && !organizerConfig_.settings.singleClickOpen) {
                    launcher_.OpenPath(item->path);
                }
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (HitTestCollapseButton(point)) {
                ToggleCollapsed();
                return 0;
            }
            if (HitTestLockButton(point)) {
                ToggleLocked();
                return 0;
            }
            const int tabIndex = HitTestTab(point);
            if (tabIndex == 0) {
                organizerConfig_.currentCategoryId = kUncategorizedCategoryId;
                RefreshCurrentItems();
                SaveOrganizerConfig();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (tabIndex > 0 && static_cast<size_t>(tabIndex - 1) < organizerConfig_.categories.size()) {
                organizerConfig_.currentCategoryId = organizerConfig_.categories[static_cast<size_t>(tabIndex - 1)].id;
                RefreshCurrentItems();
                SaveOrganizerConfig();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (tabIndex == static_cast<int>(organizerConfig_.categories.size() + 1)) {
                CreateCategory();
                return 0;
            }
            if (windowConfig_.viewMode == 1) {
                size_t tileHeaderIndex = 0;
                if (HitTestTileHeader(point, tileHeaderIndex)) {
                    organizerConfig_.currentCategoryId = tileViews_[tileHeaderIndex].categoryId;
                    const bool collapsed = !tileViews_[tileHeaderIndex].collapsed;
                    if (Category* category = FindCategory(tileViews_[tileHeaderIndex].categoryId); category != nullptr) {
                        category->tileCollapsed = collapsed;
                    } else {
                        tileCollapsed_[tileViews_[tileHeaderIndex].categoryId] = collapsed;
                    }
                    tileScrollOffset_ = std::max(0, tileScrollOffset_);
                    RefreshTileViews();
                    SaveOrganizerConfig();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                size_t tileIndex = 0;
                int tileIconIndex = -1;
                if (HitTestTileIcon(point, tileIndex, tileIconIndex)) {
                    const DesktopItem* item = tileViews_[tileIndex].grid.ItemAt(
                        static_cast<size_t>(tileIconIndex));
                    tileViews_[tileIndex].grid.SetSelectedIndex(tileIconIndex);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (organizerConfig_.settings.singleClickOpen) {
                        if (item != nullptr) {
                            launcher_.OpenPath(item->path);
                        }
                    } else if (searchQuery_.empty() &&
                               searchScope_ == 0 &&
                               item != nullptr &&
                               !DragGhostWindow::Instance().IsCommitted()) {
                        draggingTileIndex_ = static_cast<int>(tileIndex);
                        draggingTileIconIndex_ = tileIconIndex;
                        draggingItemId_ = item->id;
                        draggingSourceCategoryId_ = tileViews_[tileIndex].categoryId;
                        dragStartPoint_ = point;
                        SetCapture(hwnd_);
                    }
                }
                return 0;
            }
            const int clickedIconIndex = iconGrid_.HitTest(point);
            if (clickedIconIndex >= 0) {
                iconGrid_.SetSelectedIndex(clickedIconIndex);
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (organizerConfig_.settings.singleClickOpen) {
                    const DesktopItem* item = iconGrid_.ItemAt(static_cast<size_t>(clickedIconIndex));
                    if (item != nullptr) {
                        launcher_.OpenPath(item->path);
                    }
                    return 0;
                }
            }
            if (!windowConfig_.collapsed &&
                searchQuery_.empty() &&
                searchScope_ == 0 &&
                !DragGhostWindow::Instance().IsCommitted()) {
                const int iconIndex = iconGrid_.HitTest(point);
                const DesktopItem* dragItem = iconIndex >= 0
                    ? iconGrid_.ItemAt(static_cast<size_t>(iconIndex))
                    : nullptr;
                if (dragItem != nullptr) {
                    draggingIconIndex_ = iconIndex;
                    draggingItemId_ = dragItem->id;
                    draggingSourceCategoryId_ = organizerConfig_.currentCategoryId;
                    dragStartPoint_ = point;
                    SetCapture(hwnd_);
                    return 0;
                }
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (draggingTileIndex_ >= 0 && draggingTileIconIndex_ >= 0) {
                const std::wstring movingId = draggingItemId_;
                const std::wstring sourceCategoryId = draggingSourceCategoryId_;
                const bool completedDrag = dragVisualActive_;
                POINT releaseScreenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(hwnd_, &releaseScreenPoint);
                const std::uint64_t dragGhostGeneration =
                    completedDrag ? dragGhostGeneration_ : 0;
                POINT dropScreenPoint{};
                if (completedDrag) {
                    DragGhostWindow::Instance().Commit(releaseScreenPoint);
                    dropScreenPoint = DragGhostWindow::Instance().TopLeftScreenPoint();
                }
                bool keepDragGhost = false;
                for (TileView& tile : tileViews_) {
                    tile.grid.SetDraggingIndex(-1);
                }
                dragVisualActive_ = false;
                dragGhostGeneration_ = 0;
                draggingTileIndex_ = -1;
                draggingTileIconIndex_ = -1;
                draggingItemId_.clear();
                draggingSourceCategoryId_.clear();
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                LoadOrganizerConfig();
                if (completedDrag && !movingId.empty()) {
                    size_t targetTile = 0;
                    int targetIcon = -1;
                    if (HitTestTileIcon(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, targetTile, targetIcon)) {
                        DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
                        if (tileViews_[targetTile].categoryId == sourceCategoryId) {
                            const DesktopItem* targetItem = tileViews_[targetTile].grid.ItemAt(static_cast<size_t>(targetIcon));
                            if (targetItem != nullptr) {
                                ReorderItemInCategory(
                                    tileViews_[targetTile].categoryId,
                                    movingId,
                                    targetItem->id);
                            }
                        } else {
                            MoveItemToCategory(movingId, tileViews_[targetTile].categoryId);
                        }
                    } else {
                        size_t headerTile = 0;
                        if (HitTestTileHeader(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, headerTile) &&
                            tileViews_[headerTile].categoryId != sourceCategoryId) {
                            DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
                            MoveItemToCategory(movingId, tileViews_[headerTile].categoryId);
                        } else {
                            keepDragGhost = MoveItemOut(
                                movingId,
                                true,
                                &dropScreenPoint,
                                dragGhostGeneration);
                        }
                    }
                }
                if (!keepDragGhost) {
                    DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
                }
                FlushDeferredRefresh();
                return 0;
            }
            if (draggingIconIndex_ >= 0) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const int originIndex = draggingIconIndex_;
                const std::wstring movingId = !draggingItemId_.empty()
                    ? draggingItemId_
                    : (static_cast<size_t>(originIndex) < currentItems_.size()
                        ? currentItems_[static_cast<size_t>(originIndex)].id
                        : L"");
                const std::wstring sourceCategoryId = draggingSourceCategoryId_;
                const bool completedDrag = dragVisualActive_;
                POINT releaseScreenPoint = point;
                ClientToScreen(hwnd_, &releaseScreenPoint);
                const std::uint64_t dragGhostGeneration =
                    completedDrag ? dragGhostGeneration_ : 0;
                POINT dropScreenPoint{};
                if (completedDrag) {
                    DragGhostWindow::Instance().Commit(releaseScreenPoint);
                    dropScreenPoint = DragGhostWindow::Instance().TopLeftScreenPoint();
                }
                iconGrid_.SetDraggingIndex(-1);
                const int tabIndex = completedDrag ? HitTestTab(point) : -1;
                const RECT gridBounds = GridBounds();
                const bool droppedInsideGrid = PtInRect(&gridBounds, point) != FALSE;
                std::wstring targetCategoryId;
                if (tabIndex == 0) {
                    targetCategoryId = kUncategorizedCategoryId;
                } else if (tabIndex > 0 &&
                           tabIndex < static_cast<int>(organizerConfig_.categories.size() + 1)) {
                    targetCategoryId =
                        organizerConfig_.categories[static_cast<size_t>(tabIndex - 1)].id;
                }
                std::wstring targetItemId;
                if (droppedInsideGrid) {
                    const int targetIndex = iconGrid_.SlotIndexForPoint(point);
                    const DesktopItem* targetItem = targetIndex >= 0
                        ? iconGrid_.ItemAt(static_cast<size_t>(targetIndex))
                        : nullptr;
                    if (targetItem != nullptr) {
                        targetItemId = targetItem->id;
                    }
                }
                const bool desktopDrop =
                    completedDrag &&
                    tabIndex < 0 &&
                    !droppedInsideGrid &&
                    !movingId.empty();
                dragVisualActive_ = false;
                dragGhostGeneration_ = 0;
                draggingIconIndex_ = -1;
                draggingItemId_.clear();
                draggingSourceCategoryId_.clear();
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                LoadOrganizerConfig();
                if (!desktopDrop) {
                    DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
                }
                if (completedDrag && tabIndex >= 0 && !movingId.empty()) {
                    if (targetCategoryId == kUncategorizedCategoryId) {
                        if (sourceCategoryId != kUncategorizedCategoryId) {
                            RemoveItemFromCurrentCategory(movingId);
                        }
                    } else if (!targetCategoryId.empty() &&
                               targetCategoryId != sourceCategoryId) {
                        MoveItemToCategory(movingId, targetCategoryId);
                    }
                } else if (completedDrag) {
                    if (droppedInsideGrid) {
                        if (!sourceCategoryId.empty() &&
                            !targetItemId.empty() &&
                            targetItemId != movingId) {
                            ReorderItemInCategory(
                                sourceCategoryId,
                                movingId,
                                targetItemId);
                        }
                    } else if (!movingId.empty()) {
                        if (!MoveItemOut(
                                movingId,
                                true,
                                &dropScreenPoint,
                                dragGhostGeneration)) {
                            DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
                        }
                    }
                }
                FlushDeferredRefresh();
                return 0;
            }
            break;
        }

        case WM_RBUTTONUP: {
            POINT clientPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT screenPoint = clientPoint;
            ClientToScreen(hwnd_, &screenPoint);
            if (windowConfig_.viewMode == 1) {
                size_t tileIndex = 0;
                int tileIconIndex = -1;
                if (HitTestTileIcon(clientPoint, tileIndex, tileIconIndex)) {
                    const DesktopItem* item = tileViews_[tileIndex].grid.ItemAt(static_cast<size_t>(tileIconIndex));
                    if (item != nullptr) {
                        ShowItemMenu(screenPoint, item->id);
                    }
                } else if (HitTestTileHeader(clientPoint, tileIndex)) {
                    ShowTileMenu(screenPoint, tileIndex);
                } else {
                    ShowBackgroundMenu(screenPoint);
                }
                return 0;
            }
            const int iconIndex = iconGrid_.HitTest(clientPoint);
            if (iconIndex >= 0) {
                ShowIconMenu(screenPoint, iconIndex);
            } else {
                ShowBackgroundMenu(screenPoint);
            }
            return 0;
        }

        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT index = 0; index < count; ++index) {
                wchar_t path[MAX_PATH]{};
                if (DragQueryFileW(drop, index, path, MAX_PATH) > 0) {
                    ImportPathToCategory(path, organizerConfig_.currentCategoryId);
                }
            }
            DragFinish(drop);
            LoadDesktopItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!windowConfig_.collapsed) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd_, &point);
                RECT grid = GridBounds();
                if (PtInRect(&grid, point)) {
                    const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                    bool changed = false;
                    if (windowConfig_.viewMode == 1) {
                        const int maxScroll = std::max(0, tileContentHeight_ - static_cast<int>(grid.bottom - grid.top));
                        const int oldOffset = tileScrollOffset_;
                        tileScrollOffset_ = std::clamp(
                            tileScrollOffset_ - (wheelDelta / WHEEL_DELTA) * kWheelScrollPixels,
                            0,
                            maxScroll);
                        changed = oldOffset != tileScrollOffset_;
                        if (changed) {
                            RefreshTileViews();
                        }
                    } else {
                        changed = iconGrid_.ScrollBy(-(wheelDelta / WHEEL_DELTA) * kWheelScrollPixels);
                    }
                    if (changed) {
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        UpdateWindow(hwnd_);
                    }
                    return 0;
                }
            }
            break;
        }

        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            lastMousePoint_ = point;
            if (GetCapture() == hwnd_) {
                const bool movedEnough =
                    point.x - dragStartPoint_.x > 6 || dragStartPoint_.x - point.x > 6 ||
                    point.y - dragStartPoint_.y > 6 || dragStartPoint_.y - point.y > 6;
                if (movedEnough && draggingTileIndex_ >= 0 && draggingTileIconIndex_ >= 0 &&
                    draggingTileIndex_ < static_cast<int>(tileViews_.size())) {
                    TileView& sourceTile = tileViews_[static_cast<size_t>(draggingTileIndex_)];
                    if (!dragVisualActive_) {
                        const DesktopItem* movingItem = sourceTile.grid.ItemAt(static_cast<size_t>(draggingTileIconIndex_));
                        const RECT sourceCell = sourceTile.grid.CellAt(static_cast<size_t>(draggingTileIconIndex_));
                        if (movingItem != nullptr) {
                            POINT screenPoint = point;
                            ClientToScreen(hwnd_, &screenPoint);
                            const bool shortcut =
                                movingItem->kind == DesktopItemKind::Shortcut ||
                                movingItem->kind == DesktopItemKind::UrlShortcut;
                            dragGhostGeneration_ = DragGhostWindow::Instance().Begin(
                                instance_,
                                hwnd_,
                                movingItem->path,
                                movingItem->displayName,
                                shortcut,
                                windowConfig_.iconSize,
                                sourceTile.grid.SlotSize(),
                                POINT{dragStartPoint_.x - sourceCell.left, dragStartPoint_.y - sourceCell.top},
                                screenPoint);
                            dragVisualActive_ = DragGhostWindow::Instance().IsVisible();
                            if (!dragVisualActive_) {
                                dragGhostGeneration_ = 0;
                            }
                            if (dragVisualActive_) {
                                sourceTile.grid.SetDraggingIndex(draggingTileIconIndex_);
                            }
                        }
                    }
                    if (dragVisualActive_) {
                        POINT screenPoint = point;
                        ClientToScreen(hwnd_, &screenPoint);
                        DragGhostWindow::Instance().Update(screenPoint);
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    }
                    bool hoverChanged = false;
                    for (TileView& tile : tileViews_) {
                        hoverChanged = tile.grid.SetHoverIndex(tile.grid.HitTest(point)) || hoverChanged;
                    }
                    if (hoverChanged) {
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    return 0;
                }
                if (movedEnough && draggingIconIndex_ >= 0) {
                    if (!dragVisualActive_) {
                        const DesktopItem* movingItem = iconGrid_.ItemAt(static_cast<size_t>(draggingIconIndex_));
                        const RECT sourceCell = iconGrid_.CellAt(static_cast<size_t>(draggingIconIndex_));
                        if (movingItem != nullptr) {
                            POINT screenPoint = point;
                            ClientToScreen(hwnd_, &screenPoint);
                            const bool shortcut =
                                movingItem->kind == DesktopItemKind::Shortcut ||
                                movingItem->kind == DesktopItemKind::UrlShortcut;
                            dragGhostGeneration_ = DragGhostWindow::Instance().Begin(
                                instance_,
                                hwnd_,
                                movingItem->path,
                                movingItem->displayName,
                                shortcut,
                                windowConfig_.iconSize,
                                iconGrid_.SlotSize(),
                                POINT{dragStartPoint_.x - sourceCell.left, dragStartPoint_.y - sourceCell.top},
                                screenPoint);
                            dragVisualActive_ = DragGhostWindow::Instance().IsVisible();
                            if (!dragVisualActive_) {
                                dragGhostGeneration_ = 0;
                            }
                            if (dragVisualActive_) {
                                iconGrid_.SetDraggingIndex(draggingIconIndex_);
                            }
                        }
                    }
                    if (dragVisualActive_) {
                        POINT screenPoint = point;
                        ClientToScreen(hwnd_, &screenPoint);
                        DragGhostWindow::Instance().Update(screenPoint);
                        SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    }
                    const RECT gridBounds = GridBounds();
                    const int hoverIndex = PtInRect(&gridBounds, point) != FALSE
                        ? iconGrid_.SlotIndexForPoint(point)
                        : -1;
                    if (iconGrid_.SetHoverIndex(hoverIndex)) {
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    return 0;
                }
            }
            const int newButtonIndex = HoverButton();
            const int newTabIndex = HitTestTab(point);
            const int newIconIndex = windowConfig_.collapsed || windowConfig_.viewMode == 1 ? -1 : iconGrid_.HitTest(point);
            int newTileIndex = -1;
            if (!windowConfig_.collapsed && windowConfig_.viewMode == 1) {
                size_t tileIndex = 0;
                if (HitTestTileHeader(point, tileIndex)) {
                    newTileIndex = static_cast<int>(tileIndex);
                }
            }
            const bool changed = newButtonIndex != hoverButtonIndex_ ||
                                 newTabIndex != hoverTabIndex_ ||
                                 newTileIndex != hoverTileIndex_;
            hoverButtonIndex_ = newButtonIndex;
            hoverTabIndex_ = newTabIndex;
            hoverTileIndex_ = newTileIndex;
            iconGrid_.SetHoverIndex(newIconIndex);
            if (windowConfig_.viewMode == 1) {
                for (TileView& tile : tileViews_) {
                    tile.grid.SetHoverIndex(tile.grid.HitTest(point));
                }
            }
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd_;
            TrackMouseEvent(&track);
            if (changed || newIconIndex >= 0) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            hoverButtonIndex_ = -1;
            hoverTabIndex_ = -1;
            hoverTileIndex_ = -1;
            iconGrid_.SetHoverIndex(-1);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_TIMER:
            if (wParam == kDesktopRefreshTimerId) {
                KillTimer(hwnd_, kDesktopRefreshTimerId);
                if (draggingIconIndex_ >= 0 ||
                    draggingTileIndex_ >= 0 ||
                    dragVisualActive_ ||
                    !draggingItemId_.empty()) {
                    refreshPending_ = true;
                    return 0;
                }
                LoadDesktopItems();
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
                return 0;
            }
            break;

        case kDesktopChangedMessage:
            ScheduleDesktopRefresh();
            return 0;

        case kIconReadyMessage:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case kOrganizerConfigChangedMessage:
            if (draggingIconIndex_ >= 0 ||
                draggingTileIndex_ >= 0 ||
                dragVisualActive_ ||
                !draggingItemId_.empty()) {
                refreshPending_ = true;
                return 0;
            }
            refreshPending_ = false;
            LoadOrganizerConfig();
            windowConfig_ = organizerConfig_.window;
            LoadDesktopItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case kOrganizerConfigSyncMessage:
            // Keep the persisted item graph current while Explorer finishes
            // positioning without triggering a desktop scan or visual refresh.
            if (draggingIconIndex_ >= 0 ||
                draggingTileIndex_ >= 0 ||
                dragVisualActive_ ||
                !draggingItemId_.empty()) {
                // Every drag commit reloads the latest persisted config by ID
                // before saving; avoid replacing its live visual snapshot here.
                return 0;
            }
            LoadOrganizerConfig();
            return 0;

        case kDesktopPlacementEventMessage:
            HandleDesktopPlacementEvents(true);
            return 0;

        case kDesktopPlacementRequestMessage: {
            const auto* request = reinterpret_cast<const DesktopPlacementRequest*>(lParam);
            return request != nullptr && QueueDesktopPlacement(*request) ? 1 : 0;
        }

        case kWidgetActivateCategoryMessage: {
            const auto* categoryId = reinterpret_cast<const wchar_t*>(lParam);
            if (categoryId != nullptr && categoryId[0] != L'\0') {
                organizerConfig_.currentCategoryId = categoryId;
            }
            return 0;
        }

        case kWidgetHostCommandMessage: {
            switch (static_cast<WidgetHostCommand>(wParam)) {
                case WidgetHostCommand::CreateCategory:
                    CreateCategory();
                    break;
                case WidgetHostCommand::HideAll:
                    ToggleAllVisible();
                    break;
                case WidgetHostCommand::ToggleAllLocked:
                    ToggleAllLocked();
                    break;
                case WidgetHostCommand::RefreshAll:
                    LoadDesktopItems();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    break;
                case WidgetHostCommand::ToggleStartup:
                    ToggleStartup();
                    break;
                case WidgetHostCommand::ShowSettings:
                    ShowSettings();
                    break;
                case WidgetHostCommand::ExportConfig:
                    ExportConfig();
                    break;
                case WidgetHostCommand::ImportConfig:
                    ImportConfig();
                    break;
                case WidgetHostCommand::ImportUnassigned:
                    ImportUnassignedDesktopItems();
                    break;
                case WidgetHostCommand::ExportCategory:
                    ExportCurrentCategory();
                    break;
                case WidgetHostCommand::ImportCategory:
                    ImportCurrentCategory();
                    break;
                case WidgetHostCommand::MoveCategoryUp:
                    MoveCurrentCategory(-1);
                    break;
                case WidgetHostCommand::MoveCategoryDown:
                    MoveCurrentCategory(1);
                    break;
                case WidgetHostCommand::ExitApplication:
                    DestroyWindow(hwnd_);
                    break;
            }
            return 0;
        }

        case kTrayIconMessage:
        {
            const UINT trayEvent = LOWORD(static_cast<DWORD_PTR>(lParam));
            if (trayEvent == WM_LBUTTONDBLCLK) {
                ToggleAllVisible();
                return 0;
            }
            if (trayEvent == WM_RBUTTONUP || trayEvent == WM_CONTEXTMENU) {
                ShowTrayMenu();
                return 0;
            }
            return 0;
        }

        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lParam) == searchEdit_) {
                const bool lightTheme = UseLightTheme(organizerConfig_.settings.theme);
                static HBRUSH darkBrush = CreateSolidBrush(RGB(12, 34, 44));
                static HBRUSH lightBrush = CreateSolidBrush(RGB(247, 251, 253));
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, lightTheme ? RGB(21, 48, 59) : RGB(235, 246, 250));
                SetBkColor(dc, lightTheme ? RGB(247, 251, 253) : RGB(12, 34, 44));
                return reinterpret_cast<LRESULT>(lightTheme ? lightBrush : darkBrush);
            }
            break;

        case WM_COMMAND: {
            const int command = LOWORD(wParam);
            if (command == kSearchEditId && HIWORD(wParam) == EN_CHANGE) {
                RefreshSearchQuery();
                return 0;
            }
            if (command == kSearchScopeId && HIWORD(wParam) == CBN_SELCHANGE) {
                searchScope_ = static_cast<int>(SendMessageW(searchScopeCombo_, CB_GETCURSEL, 0, 0));
                iconGrid_.SetScrollOffset(0);
                RefreshCurrentItems();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (command == kNewCategoryCommand) {
                CreateCategory();
                return 0;
            }
            if (command == kRenameCategoryCommand) {
                RenameCurrentCategory();
                return 0;
            }
            if (command == kDeleteCategoryCommand) {
                DeleteCurrentCategory();
                return 0;
            }
            if (command == kExitCommand) {
                DestroyWindow(hwnd_);
                return 0;
            }
            if (command == kToggleCollapseCommand) {
                ToggleCollapsed();
                return 0;
            }
            if (command == kToggleLockCommand) {
                ToggleLocked();
                return 0;
            }
            if (command == kIconSmallCommand) {
                SetIconSize(36);
                return 0;
            }
            if (command == kIconMediumCommand) {
                SetIconSize(48);
                return 0;
            }
            if (command == kIconLargeCommand) {
                SetIconSize(64);
                return 0;
            }
            if (command == kImportDesktopCommand) {
                ImportUnassignedDesktopItems();
                return 0;
            }
            if (command == kRefreshDesktopCommand || command == kTrayRefreshCommand) {
                LoadDesktopItems();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (command == kOpenWidgetCommand) {
                OpenCurrentCategoryWidget();
                return 0;
            }
            if (command == kMoveCategoryUpCommand) {
                MoveCurrentCategory(-1);
                return 0;
            }
            if (command == kMoveCategoryDownCommand) {
                MoveCurrentCategory(1);
                return 0;
            }
            if (command == kCategoryColorBlueCommand) {
                SetCategoryColor(L"#2D8CFF");
                return 0;
            }
            if (command == kCategoryColorMintCommand) {
                SetCategoryColor(L"#35C9A5");
                return 0;
            }
            if (command == kCategoryColorAmberCommand) {
                SetCategoryColor(L"#F2B84B");
                return 0;
            }
            if (command == kCategoryColorVioletCommand) {
                SetCategoryColor(L"#AA7CFF");
                return 0;
            }
            if (command == kCategoryIconFolderCommand) {
                SetCategoryIcon(L"folder");
                return 0;
            }
            if (command == kCategoryIconAppsCommand) {
                SetCategoryIcon(L"apps");
                return 0;
            }
            if (command == kCategoryIconWorkCommand) {
                SetCategoryIcon(L"work");
                return 0;
            }
            if (command == kCategoryIconDocsCommand) {
                SetCategoryIcon(L"docs");
                return 0;
            }
            if (command == kCategoryIconMediaCommand) {
                SetCategoryIcon(L"media");
                return 0;
            }
            if (command == kExportCategoryCommand) {
                ExportCurrentCategory();
                return 0;
            }
            if (command == kImportCategoryCommand) {
                ImportCurrentCategory();
                return 0;
            }
            if (command == kViewTabsCommand) {
                SetViewMode(0);
                return 0;
            }
            if (command == kViewTileCommand) {
                SetViewMode(1);
                return 0;
            }
            if (command == kTabTopCommand) {
                SetTabSide(0);
                return 0;
            }
            if (command == kTabBottomCommand) {
                SetTabSide(1);
                return 0;
            }
            if (command == kTabLeftCommand) {
                SetTabSide(2);
                return 0;
            }
            if (command == kTabRightCommand) {
                SetTabSide(3);
                return 0;
            }
            if (command == kSaveLayoutProfileCommand) {
                SaveLayoutProfile();
                return 0;
            }
            if (command == kRestoreLayoutProfileCommand) {
                RestoreLayoutProfile();
                return 0;
            }
            if (command == kResetLayoutCommand) {
                ResetLayout();
                return 0;
            }
            if (command == kDensityCompactCommand) {
                SetDensity(0);
                return 0;
            }
            if (command == kDensityStandardCommand) {
                SetDensity(1);
                return 0;
            }
            if (command == kDensitySpaciousCommand) {
                SetDensity(2);
                return 0;
            }
            if (command == kTrayToggleVisibleCommand) {
                ToggleAllVisible();
                return 0;
            }
            if (command == kTrayToggleLockCommand) {
                ToggleAllLocked();
                return 0;
            }
            if (command == kTrayStartupCommand) {
                ToggleStartup();
                return 0;
            }
            if (command == kTraySettingsCommand) {
                ShowSettings();
                return 0;
            }
            if (command == kExportConfigCommand) {
                ExportConfig();
                return 0;
            }
            if (command == kImportConfigCommand) {
                ImportConfig();
                return 0;
            }
            break;
        }

        case WM_CAPTURECHANGED: {
            const bool hadActiveDrag =
                dragVisualActive_ ||
                draggingIconIndex_ >= 0 ||
                draggingTileIndex_ >= 0 ||
                draggingTileIconIndex_ >= 0 ||
                !draggingItemId_.empty();
            if (hadActiveDrag) {
                DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration_);
            }
            dragVisualActive_ = false;
            dragGhostGeneration_ = 0;
            iconGrid_.SetDraggingIndex(-1);
            if (draggingTileIndex_ >= 0 && draggingTileIndex_ < static_cast<int>(tileViews_.size())) {
                tileViews_[static_cast<size_t>(draggingTileIndex_)].grid.SetDraggingIndex(-1);
            }
            draggingIconIndex_ = -1;
            draggingTileIndex_ = -1;
            draggingTileIconIndex_ = -1;
            draggingItemId_.clear();
            draggingSourceCategoryId_.clear();
            if (hadActiveDrag) {
                FlushDeferredRefresh();
            }
            return 0;
        }

        case WM_EXITSIZEMOVE:
            SaveWindowConfig();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            Render();
            return 0;

        case WM_DESTROY:
            iconCache_.SetInvalidateCallback(nullptr);
            DragGhostWindow::Instance().End();
            desktopPlacementCoordinator_.DetachNotificationWindow(hwnd_);
            for (auto& widget : widgetWindows_) {
                if (widget != nullptr) {
                    widget->Close();
                }
            }
            widgetWindows_.clear();
            desktopWatcher_.Stop();
            trayIcon_.Remove();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void MainWindow::LoadDesktopItems() {
    DesktopScanner scanner;
    items_ = scanner.Scan(organizerConfig_.settings.showPublicDesktopItems);
    for (const RegisteredItem& registeredItem : organizerConfig_.items) {
        scanner.MergeRegisteredItem(
            items_,
            registeredItem.id,
            registeredItem.path,
            registeredItem.displayName);
    }
    iconGrid_.SetIconSize(windowConfig_.iconSize);
    iconGrid_.SetDensity(windowConfig_.density);
    iconGrid_.SetCompactStyle(false);
    iconGrid_.SetLightTheme(UseLightTheme(organizerConfig_.settings.theme));
    iconCache_.SetCapacity(static_cast<size_t>(organizerConfig_.settings.iconCacheSize));
    RefreshCurrentItems();
    iconGrid_.SetBounds(GridBounds());
    for (auto& widget : widgetWindows_) {
        if (widget != nullptr && widget->IsOpen()) {
            widget->RefreshFromConfig();
        }
    }
}

void MainWindow::ScheduleDesktopRefresh() {
    if (hwnd_ != nullptr) {
        SetTimer(hwnd_, kDesktopRefreshTimerId, 650, nullptr);
    }
}

void MainWindow::RefreshSearchQuery() {
    if (searchEdit_ == nullptr) {
        searchQuery_.clear();
    } else {
        const int length = GetWindowTextLengthW(searchEdit_);
        std::wstring value(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
        if (length > 0) {
            GetWindowTextW(searchEdit_, value.data(), length + 1);
            value.resize(static_cast<size_t>(length));
        } else {
            value.clear();
        }
        searchQuery_ = ToLowerCopy(value);
    }
    iconGrid_.SetScrollOffset(0);
    RefreshCurrentItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::LayoutSearchEdit() {
    if (searchEdit_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int clientWidth = static_cast<int>(client.right - client.left);
    if (windowConfig_.viewMode == 1) {
        if (searchScopeCombo_ != nullptr) {
            ShowWindow(searchScopeCombo_, SW_HIDE);
        }
        const int width = std::max(160, clientWidth - 126);
        SetWindowPos(searchEdit_, nullptr, 18, 7, width, 26, SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    if (searchScopeCombo_ != nullptr) {
        ShowWindow(searchScopeCombo_, SW_SHOW);
    }
    const int rightReserve = windowConfig_.tabSide == 3 ? kSideTabsWidth : 0;
    const int width = std::clamp(std::min(clientWidth / 3, clientWidth - 300 - rightReserve), 180, 300);
    SetWindowPos(searchEdit_, nullptr, 150, 7, std::max(120, width), 24, SWP_NOZORDER | SWP_NOACTIVATE);
    if (searchScopeCombo_ != nullptr) {
        SetWindowPos(searchScopeCombo_, nullptr, 158 + std::max(120, width), 7, 112, 24, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

bool MainWindow::MatchesSearch(const DesktopItem& item) const {
    if (searchQuery_.empty()) {
        return true;
    }
    const std::wstring displayName = ToLowerCopy(item.displayName);
    const std::wstring path = ToLowerCopy(item.path);
    const std::wstring targetPath = ToLowerCopy(item.targetPath);
    return displayName.find(searchQuery_) != std::wstring::npos ||
           path.find(searchQuery_) != std::wstring::npos ||
           targetPath.find(searchQuery_) != std::wstring::npos;
}

void MainWindow::ToggleAllVisible() {
    if (hwnd_ == nullptr) {
        return;
    }
    if (windowConfig_.viewMode == 1 && widgetWindows_.empty()) {
        OpenAllCategoryWidgets();
    }
    bool anyVisible = windowConfig_.viewMode == 1 ? false : IsWindowVisible(hwnd_) != FALSE;
    for (const auto& widget : widgetWindows_) {
        if (widget != nullptr && widget->IsVisible()) {
            anyVisible = true;
            break;
        }
    }
    const bool show = !anyVisible;
    if (windowConfig_.viewMode == 1) {
        ShowWindow(hwnd_, SW_HIDE);
    } else {
        ShowWindow(hwnd_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    for (auto& widget : widgetWindows_) {
        if (widget != nullptr && widget->IsOpen()) {
            widget->SetVisible(show);
        }
    }
    if (show) {
        UpdateWindow(hwnd_);
    }
    organizerConfig_.settings.lastVisible = show;
    SaveOrganizerConfig();
}

void MainWindow::ToggleStartup() {
    const bool enabled = !startupManager_.IsEnabled();
    if (!startupManager_.SetEnabled(enabled)) {
        MessageDialog::Show(instance_, hwnd_, L"无法更新开机自启设置，请稍后重试。", L"Lattice", MB_OK | MB_ICONWARNING);
        return;
    }
    organizerConfig_.settings.launchOnStartup = enabled;
    SaveOrganizerConfig();
}

void MainWindow::ShowSettings() {
    AppSettings pending = organizerConfig_.settings;
    if (!SettingsDialog::Show(instance_, hwnd_, pending)) {
        return;
    }

    if (pending.launchOnStartup != startupManager_.IsEnabled() &&
        !startupManager_.SetEnabled(pending.launchOnStartup)) {
        MessageDialog::Show(instance_, hwnd_, L"开机自启设置没有成功写入，其他设置仍会保存。", L"Lattice", MB_OK | MB_ICONWARNING);
        pending.launchOnStartup = startupManager_.IsEnabled();
    }
    const bool publicDesktopChanged = pending.showPublicDesktopItems != organizerConfig_.settings.showPublicDesktopItems;
    organizerConfig_.settings = pending;
    SaveOrganizerConfig();
    iconCache_.SetCapacity(static_cast<size_t>(organizerConfig_.settings.iconCacheSize));
    iconGrid_.SetLightTheme(UseLightTheme(organizerConfig_.settings.theme));
    if (publicDesktopChanged) {
        LoadDesktopItems();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ExportConfig() {
    SaveOrganizerConfig();
    wchar_t path[MAX_PATH] = L"Lattice-config.ini";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"Lattice 配置 (*.ini)\0*.ini\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.lpstrDefExt = L"ini";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&dialog) == FALSE) {
        return;
    }
    if (!configStore_.ExportAppConfig(path)) {
        MessageDialog::Show(instance_, hwnd_, L"配置导出失败，请选择可写的位置后重试。", L"Lattice", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::ImportConfig() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"Lattice 配置 (*.ini)\0*.ini\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog) == FALSE) {
        return;
    }
    if (MessageDialog::Show(instance_,
            hwnd_,
            L"导入配置会覆盖当前分类、布局和设置，但不会移动或删除真实桌面文件。继续吗？",
            L"导入配置",
            MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    for (auto& widget : widgetWindows_) {
        if (widget != nullptr) {
            widget->Close();
        }
    }
    widgetWindows_.clear();
    if (!configStore_.ImportAppConfig(path)) {
        MessageDialog::Show(instance_, hwnd_, L"配置文件无效或无法写入，当前配置未改变。", L"导入配置", MB_OK | MB_ICONWARNING);
        return;
    }

    LoadOrganizerConfig();
    windowConfig_ = organizerConfig_.window;
    const int height = windowConfig_.collapsed ? kTitleHeight + kTabsHeight + 12 : windowConfig_.height;
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(windowConfig_.opacity), LWA_ALPHA);
    SetWindowPos(hwnd_, nullptr, windowConfig_.x, windowConfig_.y, windowConfig_.width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    if (windowConfig_.viewMode == 1) {
        DockTileWindowToWorkArea(false);
    }
    EnsureWindowVisible();
    searchQuery_.clear();
    if (searchEdit_ != nullptr) {
        SetWindowTextW(searchEdit_, L"");
    }
    LoadDesktopItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    bool anyVisible = IsWindowVisible(hwnd_) != FALSE;
    for (const auto& widget : widgetWindows_) {
        if (widget != nullptr && widget->IsVisible()) {
            anyVisible = true;
            break;
        }
    }
    AppendMenuW(menu, MF_STRING, kTrayToggleVisibleCommand, anyVisible ? L"隐藏全部格子" : L"显示全部格子");
    AppendMenuW(menu, MF_STRING, kTrayToggleLockCommand, windowConfig_.locked ? L"解除布局锁定" : L"锁定全部布局");
    AppendMenuW(menu, MF_STRING, kTrayRefreshCommand, L"刷新桌面项目");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        menu,
        MF_STRING | (startupManager_.IsEnabled() ? MF_CHECKED : 0),
        kTrayStartupCommand,
        L"开机自启");
    AppendMenuW(menu, MF_STRING, kTraySettingsCommand, L"设置...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"退出");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd_);
    const int command = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_RETURNCMD,
        point.x,
        point.y,
        0,
        hwnd_,
        nullptr);
    if (command != 0) {
        SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void MainWindow::EnsureWindowVisible() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
    if (monitor != nullptr) {
        return;
    }

    monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = std::clamp(rect.left, info.rcWork.left, std::max(info.rcWork.left, info.rcWork.right - width));
    const int y = std::clamp(rect.top, info.rcWork.top, std::max(info.rcWork.top, info.rcWork.bottom - height));
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MainWindow::DockTileWindowToWorkArea(bool rememberRestore) {
    if (hwnd_ == nullptr) {
        return;
    }
    if (rememberRestore && !hasTileRestoreConfig_) {
        tileRestoreConfig_ = windowConfig_;
        hasTileRestoreConfig_ = true;
    }

    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
        return;
    }

    int width = windowConfig_.width;
    if (width < kTilePanelMinWidth || width > kTilePanelMaxWidth) {
        width = kTilePanelWidth;
    }
    width = std::clamp(width, kTilePanelMinWidth, kTilePanelMaxWidth);
    const int workAreaHeight = static_cast<int>(info.rcWork.bottom - info.rcWork.top);
    const int height = std::min(kTilePanelHeight, workAreaHeight);
    windowConfig_.x = info.rcWork.right - width;
    windowConfig_.y = info.rcWork.top;
    windowConfig_.width = width;
    windowConfig_.height = height;
    windowConfig_.normalHeight = height;
    SetWindowPos(
        hwnd_,
        nullptr,
        windowConfig_.x,
        windowConfig_.y,
        windowConfig_.width,
        windowConfig_.height,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

void MainWindow::LoadOrganizerConfig() {
    const AppConfig appConfig = configStore_.LoadAppConfig();
    organizerConfig_.window = appConfig.window;
    organizerConfig_.window.viewMode = 1;
    organizerConfig_.settings = appConfig.settings;
    organizerConfig_.currentCategoryId = appConfig.currentCategoryId.empty() ? kUncategorizedCategoryId : appConfig.currentCategoryId;
    organizerConfig_.uncategorizedName = appConfig.uncategorizedName.empty() ? L"未分类" : appConfig.uncategorizedName;
    organizerConfig_.uncategorizedStorageFolder = appConfig.uncategorizedStorageFolder.empty()
        ? std::wstring(kUncategorizedCategoryId)
        : appConfig.uncategorizedStorageFolder;
    organizerConfig_.items.clear();
    for (const ItemConfig& itemConfig : appConfig.items) {
        RegisteredItem item;
        item.id = itemConfig.id;
        item.path = itemConfig.path;
        item.displayName = itemConfig.displayName;
        item.originalDesktopPath = itemConfig.originalDesktopPath;
        item.desktopX = itemConfig.desktopX;
        item.desktopY = itemConfig.desktopY;
        item.hasDesktopPosition = itemConfig.hasDesktopPosition;
        organizerConfig_.items.push_back(std::move(item));
    }
    organizerConfig_.uncategorizedItemIds = appConfig.uncategorizedItemIds;
    organizerConfig_.categories.clear();
    for (const CategoryConfig& categoryConfig : appConfig.categories) {
        Category category;
        category.id = categoryConfig.id;
        category.name = categoryConfig.name;
        category.storageFolder = categoryConfig.storageFolder.empty() ? categoryConfig.id : categoryConfig.storageFolder;
        category.itemIds = categoryConfig.itemIds;
        category.color = categoryConfig.color;
        category.icon = categoryConfig.icon;
        category.tileCollapsed = categoryConfig.tileCollapsed;
        category.layout = categoryConfig.layout;
        organizerConfig_.categories.push_back(std::move(category));
    }
    if (organizerConfig_.currentCategoryId != kUncategorizedCategoryId &&
        FindCategory(organizerConfig_.currentCategoryId) == nullptr) {
        organizerConfig_.currentCategoryId = kUncategorizedCategoryId;
    }
}

RECT MainWindow::GridBounds() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (windowConfig_.viewMode == 1) {
        client.left += 12;
        client.right -= 12;
        client.top += kTitleHeight + 8;
        client.bottom -= 12;
        if (client.right <= client.left) {
            client.right = client.left + 1;
        }
        if (client.bottom <= client.top) {
            client.bottom = client.top + 1;
        }
        return client;
    }
    const int side = std::clamp(windowConfig_.tabSide, 0, 3);
    client.left += side == 2 ? kSideTabsWidth : 22;
    client.right -= side == 3 ? kSideTabsWidth : 18;
    client.top += side >= 2 ? kTitleHeight + 8 : 84;
    client.bottom -= side == 1 ? kTabsHeight + 16 : 18;
    if (client.right <= client.left) {
        client.right = client.left + 1;
    }
    if (client.bottom <= client.top) {
        client.bottom = client.top + 1;
    }
    return client;
}

RECT MainWindow::CollapseButtonBounds() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    return RECT{client.right - 82, 8, client.right - 52, 32};
}

RECT MainWindow::LockButtonBounds() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    return RECT{client.right - 46, 8, client.right - 16, 32};
}

RECT MainWindow::TabBounds(size_t index) const {
    const size_t newIndex = organizerConfig_.categories.size() + 1;
    const int side = std::clamp(windowConfig_.tabSide, 0, 3);
    const int tabExtent = index == newIndex ? 38 : 112;
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (side == 2 || side == 3) {
        const int width = std::min(kSideTabsWidth - 16, 124);
        const int top = kTitleHeight + 8 + static_cast<int>(index) * (kTabsHeight + 6);
        const int left = side == 2 ? 8 : std::max(8, static_cast<int>(client.right) - width - 8);
        return RECT{left, top, left + width, top + kTabsHeight};
    }

    int x = 18;
    for (size_t i = 0; i < index; ++i) {
        x += (i == newIndex ? 38 : 112) + 8;
    }
    const int y = side == 1 ? static_cast<int>(client.bottom) - kTabsHeight : kTabsTop;
    return RECT{x, y + 4, x + tabExtent, y + kTabsHeight - 4};
}

RECT MainWindow::TileHeaderBounds(size_t index) const {
    if (index >= tileViews_.size()) {
        return RECT{};
    }
    const RECT& bounds = tileViews_[index].bounds;
    return RECT{bounds.left + 1, bounds.top + 1, bounds.right - 1, bounds.top + kTileHeaderHeight - 1};
}

void MainWindow::Render() {
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);

    if (d2d_.Target() == nullptr) {
        d2d_.RecreateTarget(hwnd_);
    }

    d2d_.BeginDraw();
    ID2D1HwndRenderTarget* newTarget = d2d_.Target();
    if (newTarget != nullptr) {
        newTarget->Clear(D2D1::ColorF(0x071720, 0.0f));

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> headerBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelLeftBorderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelTopBorderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelShadowBorderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mutedBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
        const bool lightTheme = UseLightTheme(organizerConfig_.settings.theme);
        const FLOAT titleAlpha = static_cast<FLOAT>(std::clamp(windowConfig_.titleOpacity, 80, 255)) / 255.0f;
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0xF7FBFD : 0x0A252F, 1.0f), panelBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0xEAF2F5 : 0x071C25, titleAlpha), headerBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x4C90A3 : 0x60B1C9, lightTheme ? 0.46f : 0.90f),
            borderBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x4C90A3 : 0x5EA6BE, lightTheme ? 0.46f : 1.0f),
            panelLeftBorderBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x4C90A3 : 0x3A6A7A, lightTheme ? 0.46f : 1.0f),
            panelTopBorderBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x4C90A3 : 0x5AA1BB, lightTheme ? 0.46f : 1.0f),
            panelShadowBorderBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x15303B : 0xF6FAFF, 1.0f), textBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x507080 : 0x96B9C6, 1.0f), mutedBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0xD7EDF2 : 0x24566A, 0.96f), hoverBrush.GetAddressOf());

        const D2D1_SIZE_F size = newTarget->GetSize();
        const D2D1_RECT_F panelRect = D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f);
        newTarget->FillRectangle(panelRect, panelBrush.Get());
        newTarget->FillRectangle(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, static_cast<FLOAT>(kTitleHeight)), headerBrush.Get());
        if (windowConfig_.showBorder) {
            newTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            newTarget->FillRectangle(
                D2D1::RectF(0.0f, 0.0f, 1.0f, size.height),
                panelLeftBorderBrush.Get());
            newTarget->FillRectangle(
                D2D1::RectF(size.width - 1.0f, 0.0f, size.width, size.height),
                panelShadowBorderBrush.Get());
            newTarget->FillRectangle(
                D2D1::RectF(0.0f, 0.0f, size.width, 1.0f),
                panelTopBorderBrush.Get());
            newTarget->FillRectangle(
                D2D1::RectF(0.0f, size.height - 1.0f, size.width, size.height),
                panelShadowBorderBrush.Get());
            newTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            newTarget->DrawLine(
                D2D1::Point2F(0.0f, static_cast<FLOAT>(kTitleHeight)),
                D2D1::Point2F(size.width, static_cast<FLOAT>(kTitleHeight)),
                borderBrush.Get(),
                1.0f);
        }

        Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat;
        d2d_.WriteFactory()->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            17.0f,
            L"zh-cn",
            titleFormat.GetAddressOf());
        if (titleFormat != nullptr && windowConfig_.viewMode != 1) {
            titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const wchar_t title[] = L"Lattice";
            newTarget->DrawTextW(
                title,
                static_cast<UINT32>(wcslen(title)),
                titleFormat.Get(),
                D2D1::RectF(0.0f, 0.0f, size.width, static_cast<FLOAT>(kTitleHeight)),
                textBrush.Get());
        }

        auto drawHeaderButton = [&](RECT rect, const std::wstring& assetName, const wchar_t* fallbackLabel, int buttonIndex) {
            const D2D1_RECT_F buttonRect = D2D1::RectF(
                static_cast<FLOAT>(rect.left),
                static_cast<FLOAT>(rect.top),
                static_cast<FLOAT>(rect.right),
                static_cast<FLOAT>(rect.bottom));
            newTarget->FillRoundedRectangle(
                D2D1::RoundedRect(buttonRect, 5.0f, 5.0f),
                hoverButtonIndex_ == buttonIndex ? hoverBrush.Get() : panelBrush.Get());
            if (windowConfig_.showBorder) {
                newTarget->DrawRoundedRectangle(D2D1::RoundedRect(buttonRect, 5.0f, 5.0f), borderBrush.Get(), 1.0f);
            }
            (void)assetName;
            if (titleFormat != nullptr) {
                newTarget->DrawTextW(
                    fallbackLabel,
                    static_cast<UINT32>(wcslen(fallbackLabel)),
                    titleFormat.Get(),
                    buttonRect,
                    textBrush.Get());
            }
        };
        drawHeaderButton(
            CollapseButtonBounds(),
            windowConfig_.collapsed ? L"btn_expand_@125.png" : L"btn_collapse_@125.png",
            windowConfig_.collapsed ? L"▾" : L"▴",
            1);
        drawHeaderButton(
            LockButtonBounds(),
            windowConfig_.locked ? L"btn_lock_@125.png" : L"btn_unlock_@125.png",
            windowConfig_.locked ? L"锁" : L"解",
            2);

        Microsoft::WRL::ComPtr<IDWriteTextFormat> tabFormat;
        d2d_.WriteFactory()->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            13.0f,
            L"zh-cn",
            tabFormat.GetAddressOf());
        if (tabFormat != nullptr) {
            tabFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tabFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> activeTabBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> inactiveTabBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverTabBrush;
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0xBFE3EC : 0x26708F, 0.94f), activeTabBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0xE4EEF2 : 0x102934, 0.84f), inactiveTabBrush.GetAddressOf());
        newTarget->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0xD7EDF2 : 0x1A4659, 0.92f), hoverTabBrush.GetAddressOf());

        if (windowConfig_.viewMode != 1) {
            const size_t tabCount = organizerConfig_.categories.size() + 2;
            for (size_t index = 0; index < tabCount; ++index) {
            RECT tabRectWin = TabBounds(index);
            D2D1_RECT_F tabRect = D2D1::RectF(
                static_cast<FLOAT>(tabRectWin.left),
                static_cast<FLOAT>(tabRectWin.top),
                static_cast<FLOAT>(tabRectWin.right),
                static_cast<FLOAT>(tabRectWin.bottom));

            std::wstring label;
            bool active = false;
            if (index == 0) {
                label = L"未分类";
                active = organizerConfig_.currentCategoryId == kUncategorizedCategoryId;
            } else if (index == tabCount - 1) {
                label = L"+";
            } else {
                const Category& category = organizerConfig_.categories[index - 1];
                label = category.icon.empty() ? L"" : category.icon.substr(0, 1);
                if (!label.empty()) {
                    label += L" ";
                }
                label += category.name;
                active = organizerConfig_.currentCategoryId == category.id;
            }

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> categoryBrush;
            ID2D1SolidColorBrush* tabFill = nullptr;
            if (active && index > 0 && index < tabCount - 1) {
                newTarget->CreateSolidColorBrush(
                    CategoryAccentColor(organizerConfig_.categories[index - 1].color, 0.94f),
                    categoryBrush.GetAddressOf());
                tabFill = categoryBrush.Get();
            } else if (active) {
                tabFill = activeTabBrush.Get();
            } else if (hoverTabIndex_ == static_cast<int>(index)) {
                tabFill = hoverTabBrush.Get();
            } else {
                tabFill = inactiveTabBrush.Get();
            }
            newTarget->FillRoundedRectangle(D2D1::RoundedRect(tabRect, 5.0f, 5.0f), tabFill);
            if (windowConfig_.showBorder) {
                newTarget->DrawRoundedRectangle(D2D1::RoundedRect(tabRect, 5.0f, 5.0f), borderBrush.Get(), 1.0f);
            }
            if (tabFormat != nullptr) {
                newTarget->DrawTextW(
                    label.c_str(),
                    static_cast<UINT32>(label.size()),
                    tabFormat.Get(),
                    tabRect,
                    textBrush.Get());
            }
            }
        }

        if (!windowConfig_.collapsed && windowConfig_.viewMode == 1) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> tileSurfaceBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> tileExpandedBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> tileHoverBrush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> tileBorderBrush;
            newTarget->CreateSolidColorBrush(
                D2D1::ColorF(lightTheme ? 0xFFFFFF : 0x0F2F39, lightTheme ? 0.78f : 1.0f),
                tileSurfaceBrush.GetAddressOf());
            newTarget->CreateSolidColorBrush(
                D2D1::ColorF(lightTheme ? 0xFFFFFF : 0x234E5E, lightTheme ? 0.82f : 1.0f),
                tileExpandedBrush.GetAddressOf());
            newTarget->CreateSolidColorBrush(
                D2D1::ColorF(lightTheme ? 0xE7F4F7 : 0x1A4A59, lightTheme ? 0.96f : 0.84f),
                tileHoverBrush.GetAddressOf());
            newTarget->CreateSolidColorBrush(
                D2D1::ColorF(lightTheme ? 0xAFC3CA : 0x7199A4, lightTheme ? 0.46f : 0.36f),
                tileBorderBrush.GetAddressOf());

            Microsoft::WRL::ComPtr<IDWriteTextFormat> tileFormat;
            d2d_.WriteFactory()->CreateTextFormat(
                L"Microsoft YaHei UI",
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                14.0f,
                L"zh-cn",
                tileFormat.GetAddressOf());
            if (tileFormat != nullptr) {
                tileFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                tileFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            Microsoft::WRL::ComPtr<IDWriteTextFormat> tileCountFormat;
            d2d_.WriteFactory()->CreateTextFormat(
                L"Microsoft YaHei UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                12.0f,
                L"zh-cn",
                tileCountFormat.GetAddressOf());
            if (tileCountFormat != nullptr) {
                tileCountFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                tileCountFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            const RECT tileArea = GridBounds();
            newTarget->PushAxisAlignedClip(
                D2D1::RectF(
                    static_cast<FLOAT>(tileArea.left),
                    static_cast<FLOAT>(tileArea.top),
                    static_cast<FLOAT>(tileArea.right),
                    static_cast<FLOAT>(tileArea.bottom)),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (TileView& tile : tileViews_) {
                const D2D1_RECT_F tileRect = D2D1::RectF(
                    static_cast<FLOAT>(tile.bounds.left),
                    static_cast<FLOAT>(tile.bounds.top),
                    static_cast<FLOAT>(tile.bounds.right),
                    static_cast<FLOAT>(tile.bounds.bottom));
                newTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(tileRect, 5.0f, 5.0f),
                    hoverTileIndex_ >= 0 && &tile == &tileViews_[static_cast<size_t>(hoverTileIndex_)]
                        ? tileHoverBrush.Get()
                        : tile.collapsed ? tileSurfaceBrush.Get() : tileExpandedBrush.Get());
                if (windowConfig_.showBorder) {
                    newTarget->DrawRoundedRectangle(
                        D2D1::RoundedRect(tileRect, 5.0f, 5.0f),
                        tileBorderBrush.Get(),
                        1.0f);
                }
                const D2D1_RECT_F accentRect = D2D1::RectF(
                    static_cast<FLOAT>(tile.bounds.left + 10),
                    static_cast<FLOAT>(tile.bounds.top + 10),
                    static_cast<FLOAT>(tile.bounds.left + 13),
                    static_cast<FLOAT>(tile.bounds.top + kTileHeaderHeight - 10));
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accentBrush;
                newTarget->CreateSolidColorBrush(CategoryAccentColor(tile.color, 0.86f), accentBrush.GetAddressOf());
                newTarget->FillRoundedRectangle(D2D1::RoundedRect(accentRect, 1.5f, 1.5f), accentBrush.Get());

                if (tileFormat != nullptr) {
                    std::wstring tileLabel = tile.collapsed ? L"\u25b8 " : L"\u25be ";
                    tileLabel += tile.name;
                    newTarget->DrawTextW(
                        tileLabel.c_str(),
                        static_cast<UINT32>(tileLabel.size()),
                        tileFormat.Get(),
                        D2D1::RectF(
                            static_cast<FLOAT>(tile.bounds.left + 22),
                            static_cast<FLOAT>(tile.bounds.top + 1),
                            static_cast<FLOAT>(tile.bounds.right - 66),
                            static_cast<FLOAT>(tile.bounds.top + kTileHeaderHeight - 1)),
                        textBrush.Get());
                }
                if (tileCountFormat != nullptr) {
                    const std::wstring countLabel = tile.itemCount == 0
                                                        ? L"空分类"
                                                        : std::to_wstring(tile.itemCount) + L" 项";
                    newTarget->DrawTextW(
                        countLabel.c_str(),
                        static_cast<UINT32>(countLabel.size()),
                        tileCountFormat.Get(),
                        D2D1::RectF(
                            static_cast<FLOAT>(tile.bounds.left + 22),
                            static_cast<FLOAT>(tile.bounds.top + 1),
                            static_cast<FLOAT>(tile.bounds.right - 22),
                            static_cast<FLOAT>(tile.bounds.top + kTileHeaderHeight - 1)),
                        mutedBrush.Get());
                }
                if (windowConfig_.showBorder) {
                    newTarget->DrawLine(
                        D2D1::Point2F(
                            static_cast<FLOAT>(tile.bounds.left + 14),
                            static_cast<FLOAT>(tile.bounds.top + kTileHeaderHeight)),
                        D2D1::Point2F(
                            static_cast<FLOAT>(tile.bounds.right - 14),
                            static_cast<FLOAT>(tile.bounds.top + kTileHeaderHeight)),
                        tileBorderBrush.Get(),
                        1.0f);
                }
                if (!tile.collapsed) {
                    if (tile.itemCount == 0) {
                        const wchar_t emptyText[] = L"拖入项目到此分类";
                        if (tileFormat != nullptr) {
                            newTarget->DrawTextW(
                                emptyText,
                                static_cast<UINT32>(wcslen(emptyText)),
                                tileFormat.Get(),
                                D2D1::RectF(
                                    static_cast<FLOAT>(tile.bounds.left + 24),
                                    static_cast<FLOAT>(tile.bounds.top + kTileHeaderHeight + 24),
                                    static_cast<FLOAT>(tile.bounds.right - 24),
                                    static_cast<FLOAT>(tile.bounds.bottom - 18)),
                                mutedBrush.Get());
                        }
                    } else {
                        tile.grid.Draw(d2d_, iconCache_);
                    }
                }
            }
            newTarget->PopAxisAlignedClip();
        }

        if (!windowConfig_.collapsed && windowConfig_.viewMode == 0) {
            if (currentItems_.empty()) {
                Microsoft::WRL::ComPtr<IDWriteTextFormat> emptyFormat;
                d2d_.WriteFactory()->CreateTextFormat(
                    L"Microsoft YaHei UI",
                    nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    15.0f,
                    L"zh-cn",
                    emptyFormat.GetAddressOf());
                if (emptyFormat != nullptr) {
                    emptyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    emptyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    RECT grid = GridBounds();
                    const D2D1_RECT_F emptyRect = D2D1::RectF(
                        static_cast<FLOAT>(grid.left + 32),
                        static_cast<FLOAT>(grid.top + 44),
                        static_cast<FLOAT>(grid.right - 32),
                        static_cast<FLOAT>(grid.top + 130));
                    if (windowConfig_.showBorder) {
                        newTarget->DrawRoundedRectangle(D2D1::RoundedRect(emptyRect, 10.0f, 10.0f), borderBrush.Get(), 1.2f);
                    }
                    const wchar_t emptyText[] = L"拖入快捷方式或文件";
                    newTarget->DrawTextW(
                        emptyText,
                        static_cast<UINT32>(wcslen(emptyText)),
                        emptyFormat.Get(),
                        emptyRect,
                        mutedBrush.Get());
                }
            } else {
                iconGrid_.Draw(d2d_, iconCache_);
            }
        }

        if (!windowConfig_.collapsed && !windowConfig_.locked && windowConfig_.showBorder) {
            newTarget->DrawLine(
                D2D1::Point2F(size.width - 16.0f, size.height - 4.0f),
                D2D1::Point2F(size.width - 4.0f, size.height - 16.0f),
                borderBrush.Get(),
                1.0f);
            newTarget->DrawLine(
                D2D1::Point2F(size.width - 10.0f, size.height - 4.0f),
                D2D1::Point2F(size.width - 4.0f, size.height - 10.0f),
                borderBrush.Get(),
                1.0f);
        }
    }

    const HRESULT newHr = d2d_.EndDraw();
    if (newHr == D2DERR_RECREATE_TARGET) {
        iconCache_.Clear();
        d2d_.RecreateTarget(hwnd_);
    }

    EndPaint(hwnd_, &paint);
    return;

#if 0
    if (d2d_.Target() == nullptr) {
        d2d_.RecreateTarget(hwnd_);
    }

    d2d_.BeginDraw();
    ID2D1HwndRenderTarget* target = d2d_.Target();
    if (target != nullptr) {
        target->Clear(D2D1::ColorF(0x071720));

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
        target->CreateSolidColorBrush(D2D1::ColorF(0x0E3545, 0.88f), panelBrush.GetAddressOf());
        target->CreateSolidColorBrush(D2D1::ColorF(0x71C4E8, 0.7f), borderBrush.GetAddressOf());
        target->CreateSolidColorBrush(D2D1::ColorF(0xF6FAFF, 1.0f), textBrush.GetAddressOf());

        const D2D1_SIZE_F size = target->GetSize();
        const D2D1_RECT_F panelRect = D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f);
        target->FillRectangle(panelRect, panelBrush.Get());
        target->DrawRectangle(panelRect, borderBrush.Get(), 1.0f);
        target->DrawLine(
            D2D1::Point2F(0.0f, static_cast<FLOAT>(kTitleHeight)),
            D2D1::Point2F(size.width, static_cast<FLOAT>(kTitleHeight)),
            borderBrush.Get(),
            1.0f);

        Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat;
        d2d_.WriteFactory()->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            17.0f,
            L"zh-cn",
            titleFormat.GetAddressOf());
        if (titleFormat != nullptr) {
            titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const wchar_t title[] = L"Lattice";
            target->DrawTextW(
                title,
                static_cast<UINT32>(wcslen(title)),
                titleFormat.Get(),
                D2D1::RectF(0.0f, 0.0f, size.width, static_cast<FLOAT>(kTitleHeight)),
                textBrush.Get());
        }

        Microsoft::WRL::ComPtr<IDWriteTextFormat> tabFormat;
        d2d_.WriteFactory()->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            13.0f,
            L"zh-cn",
            tabFormat.GetAddressOf());
        if (tabFormat != nullptr) {
            tabFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tabFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> activeTabBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> inactiveTabBrush;
        target->CreateSolidColorBrush(D2D1::ColorF(0x26708F, 0.92f), activeTabBrush.GetAddressOf());
        target->CreateSolidColorBrush(D2D1::ColorF(0x102934, 0.82f), inactiveTabBrush.GetAddressOf());

        const size_t tabCount = organizerConfig_.categories.size() + 2;
        for (size_t index = 0; index < tabCount; ++index) {
            RECT tabRectWin = TabBounds(index);
            D2D1_RECT_F tabRect = D2D1::RectF(
                static_cast<FLOAT>(tabRectWin.left),
                static_cast<FLOAT>(tabRectWin.top),
                static_cast<FLOAT>(tabRectWin.right),
                static_cast<FLOAT>(tabRectWin.bottom));

            std::wstring label;
            bool active = false;
            if (index == 0) {
                label = L"未分类";
                active = organizerConfig_.currentCategoryId == kUncategorizedCategoryId;
            } else if (index == tabCount - 1) {
                label = L"+";
            } else {
                const Category& category = organizerConfig_.categories[index - 1];
                label = category.name;
                active = organizerConfig_.currentCategoryId == category.id;
            }

            target->FillRoundedRectangle(
                D2D1::RoundedRect(tabRect, 5.0f, 5.0f),
                active ? activeTabBrush.Get() : inactiveTabBrush.Get());
            target->DrawRoundedRectangle(D2D1::RoundedRect(tabRect, 5.0f, 5.0f), borderBrush.Get(), 1.0f);
            if (tabFormat != nullptr) {
                target->DrawTextW(
                    label.c_str(),
                    static_cast<UINT32>(label.size()),
                    tabFormat.Get(),
                    tabRect,
                    textBrush.Get());
            }
        }

        iconGrid_.Draw(d2d_, iconCache_);

        target->DrawLine(
            D2D1::Point2F(size.width - 16.0f, size.height - 4.0f),
            D2D1::Point2F(size.width - 4.0f, size.height - 16.0f),
            borderBrush.Get(),
            1.0f);
        target->DrawLine(
            D2D1::Point2F(size.width - 10.0f, size.height - 4.0f),
            D2D1::Point2F(size.width - 4.0f, size.height - 10.0f),
            borderBrush.Get(),
            1.0f);
    }

    const HRESULT hr = d2d_.EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        iconCache_.Clear();
        d2d_.RecreateTarget(hwnd_);
    }

    EndPaint(hwnd_, &paint);
#endif
}

void MainWindow::SaveWindowConfig() {
    if (hwnd_ == nullptr) {
        return;
    }

    if (windowConfig_.viewMode == 1) {
        LoadOrganizerConfig();
        windowConfig_ = organizerConfig_.window;
        return;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd_, &rect)) {
        return;
    }

    windowConfig_.x = rect.left;
    windowConfig_.y = rect.top;
    windowConfig_.width = rect.right - rect.left;
    windowConfig_.height = rect.bottom - rect.top;
    windowConfig_.dpi = GetDpiForWindow(hwnd_);
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        windowConfig_.monitorId = monitorInfo.szDevice;
    }
    if (!windowConfig_.collapsed) {
        windowConfig_.normalHeight = windowConfig_.height;
    }
    organizerConfig_.window = windowConfig_;
    SaveOrganizerConfig();
}

bool MainWindow::SaveOrganizerConfig() {
    AppConfig appConfig = configStore_.LoadAppConfig();
    appConfig.settings = organizerConfig_.settings;
    appConfig.window = windowConfig_;
    appConfig.currentCategoryId = organizerConfig_.currentCategoryId;
    appConfig.uncategorizedName = organizerConfig_.uncategorizedName;
    appConfig.uncategorizedStorageFolder = organizerConfig_.uncategorizedStorageFolder;
    appConfig.items.clear();
    for (const RegisteredItem& item : organizerConfig_.items) {
        ItemConfig itemConfig;
        itemConfig.id = item.id;
        itemConfig.path = item.path;
        itemConfig.displayName = item.displayName;
        itemConfig.originalDesktopPath = item.originalDesktopPath;
        itemConfig.desktopX = item.desktopX;
        itemConfig.desktopY = item.desktopY;
        itemConfig.hasDesktopPosition = item.hasDesktopPosition;
        appConfig.items.push_back(std::move(itemConfig));
    }
    for (const RegisteredItem& item : organizerConfig_.items) {
        if (!item.hasDesktopPosition || item.originalDesktopPath.empty()) {
            continue;
        }
        auto placement = std::find_if(
            appConfig.desktopLayout.begin(),
            appConfig.desktopLayout.end(),
            [&](const DesktopPlacementConfig& value) {
                return CompareStringOrdinal(
                           value.path.c_str(), -1, item.originalDesktopPath.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        if (placement == appConfig.desktopLayout.end()) {
            appConfig.desktopLayout.push_back(
                DesktopPlacementConfig{item.originalDesktopPath, item.desktopX, item.desktopY});
        } else {
            placement->x = item.desktopX;
            placement->y = item.desktopY;
        }
    }
    appConfig.uncategorizedItemIds = organizerConfig_.uncategorizedItemIds;
    appConfig.categories.clear();
    for (const Category& category : organizerConfig_.categories) {
        CategoryConfig categoryConfig;
        categoryConfig.id = category.id;
        categoryConfig.name = category.name;
        categoryConfig.storageFolder = category.storageFolder.empty() ? category.id : category.storageFolder;
        categoryConfig.itemIds = category.itemIds;
        categoryConfig.color = category.color;
        categoryConfig.icon = category.icon;
        categoryConfig.tileCollapsed = category.tileCollapsed;
        categoryConfig.layout = category.layout;
        appConfig.categories.push_back(std::move(categoryConfig));
    }
    if (!configStore_.SaveAppConfig(appConfig)) {
        return false;
    }
    for (auto& widget : widgetWindows_) {
        if (widget != nullptr && widget->IsOpen()) {
            widget->RefreshFromConfig();
        }
    }
    return true;
}

int MainWindow::HitTestTab(POINT point) const {
    if (windowConfig_.viewMode == 1) {
        return -1;
    }
    const size_t tabCount = organizerConfig_.categories.size() + 2;
    for (size_t index = 0; index < tabCount; ++index) {
        RECT tab = TabBounds(index);
        if (PtInRect(&tab, point)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool MainWindow::HitTestCollapseButton(POINT point) const {
    RECT rect = CollapseButtonBounds();
    return PtInRect(&rect, point) != FALSE;
}

bool MainWindow::HitTestLockButton(POINT point) const {
    RECT rect = LockButtonBounds();
    return PtInRect(&rect, point) != FALSE;
}

int MainWindow::HoverButton() const {
    if (HitTestCollapseButton(lastMousePoint_)) {
        return 1;
    }
    if (HitTestLockButton(lastMousePoint_)) {
        return 2;
    }
    return -1;
}

void MainWindow::RefreshCurrentItems() {
    std::vector<DesktopItem> nextItems;
    if (searchScope_ == 1) {
        nextItems = items_;
    } else if (searchScope_ == 2 || organizerConfig_.currentCategoryId == kUncategorizedCategoryId) {
        for (const std::wstring& itemId : organizerConfig_.uncategorizedItemIds) {
            const auto found = std::find_if(items_.begin(), items_.end(), [&](const DesktopItem& item) {
                return item.id == itemId;
            });
            if (found != items_.end()) {
                nextItems.push_back(*found);
            }
        }
    } else {
        const Category* category = FindCategory(organizerConfig_.currentCategoryId);
        if (category != nullptr) {
            for (const std::wstring& itemId : category->itemIds) {
                const auto found = std::find_if(items_.begin(), items_.end(), [&](const DesktopItem& item) {
                    return item.id == itemId;
                });
                if (found != items_.end()) {
                    nextItems.push_back(*found);
                }
            }
        }
    }
    nextItems.erase(
        std::remove_if(nextItems.begin(), nextItems.end(), [&](const DesktopItem& item) {
            return !MatchesSearch(item);
        }),
        nextItems.end());
    const bool changed = !SameDesktopItems(currentItems_, nextItems);
    currentItems_ = std::move(nextItems);
    if (changed) {
        iconGrid_.SetItems(currentItems_);
    }
    RefreshTileViews();
}

void MainWindow::RefreshTileViews() {
    tileViews_.clear();
    tileContentHeight_ = 0;
    hoverTileIndex_ = -1;
    if (windowConfig_.viewMode != 1 || hwnd_ == nullptr) {
        return;
    }

    struct Source {
        std::wstring id;
        std::wstring name;
        std::wstring color;
        const std::vector<std::wstring>* itemIds = nullptr;
    };
    std::vector<Source> sources;
    if (searchScope_ != 2) {
        sources.push_back(Source{kUncategorizedCategoryId, L"\u672a\u5206\u7c7b", L"#2D8CFF", &organizerConfig_.uncategorizedItemIds});
    }
    if (searchScope_ != 2) {
        for (size_t categoryIndex = 0; categoryIndex < organizerConfig_.categories.size(); ++categoryIndex) {
            const Category& category = organizerConfig_.categories[categoryIndex];
            sources.push_back(Source{
                category.id,
                category.name,
                EffectiveCategoryColor(category.color, categoryIndex),
                &category.itemIds});
        }
    }
    if (searchScope_ == 2) {
        sources.push_back(Source{kUncategorizedCategoryId, L"\u672a\u5206\u7c7b", L"#2D8CFF", &organizerConfig_.uncategorizedItemIds});
    }

    const RECT content = GridBounds();
    const int tileWidth = std::max(220, static_cast<int>(content.right - content.left));
    int cursorTop = content.top;
    for (size_t index = 0; index < sources.size(); ++index) {
        const Source& source = sources[index];
        TileView view;
        view.categoryId = source.id;
        view.name = source.name;
        view.color = source.color;
        const Category* category = FindCategory(source.id);
        if (category != nullptr) {
            view.collapsed = category->tileCollapsed;
        } else {
            const auto collapsedIt = tileCollapsed_.find(source.id);
            view.collapsed = collapsedIt != tileCollapsed_.end() && collapsedIt->second;
        }
        const int left = content.left;
        const int top = cursorTop;
        std::vector<DesktopItem> viewItems;
        if (source.itemIds != nullptr) {
            for (const std::wstring& itemId : *source.itemIds) {
                const auto found = std::find_if(items_.begin(), items_.end(), [&](const DesktopItem& item) {
                    return item.id == itemId && MatchesSearch(item);
                });
                if (found != items_.end()) {
                    viewItems.push_back(*found);
                }
            }
        }
        view.itemCount = viewItems.size();
        const int tileHeight = view.collapsed ? kTileHeaderHeight : kTileExpandedHeight;
        view.bounds = RECT{left, top, left + tileWidth, top + tileHeight};
        view.grid.SetIconSize(std::max(64, windowConfig_.iconSize));
        view.grid.SetDensity(0);
        view.grid.SetCompactStyle(true);
        view.grid.SetLightTheme(UseLightTheme(organizerConfig_.settings.theme));
        view.grid.SetItems(std::move(viewItems));
        view.grid.SetBounds(view.collapsed
                                ? RECT{left + 12, top + kTileHeaderHeight, left + tileWidth - 12, top + kTileHeaderHeight + 1}
                                : RECT{left + 12, top + kTileHeaderHeight, left + tileWidth - 12, top + tileHeight - 10});
        tileViews_.push_back(std::move(view));
        tileViews_.back().headerBounds = TileHeaderBounds(tileViews_.size() - 1);
        cursorTop += tileHeight + kTileRowGap;
    }

    const int contentHeight = static_cast<int>(cursorTop - content.top - kTileRowGap);
    tileContentHeight_ = std::max(0, contentHeight);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    const int maxScroll = std::max(0, tileContentHeight_ - visibleHeight);
    tileScrollOffset_ = std::clamp(tileScrollOffset_, 0, maxScroll);
    if (tileScrollOffset_ != 0) {
        for (TileView& tile : tileViews_) {
            tile.bounds.top -= tileScrollOffset_;
            tile.bounds.bottom -= tileScrollOffset_;
            tile.headerBounds = TileHeaderBounds(static_cast<size_t>(&tile - tileViews_.data()));
            tile.grid.SetBounds(tile.collapsed
                                    ? RECT{tile.bounds.left + 12, tile.bounds.top + kTileHeaderHeight, tile.bounds.right - 12, tile.bounds.top + kTileHeaderHeight + 1}
                                    : RECT{tile.bounds.left + 12, tile.bounds.top + kTileHeaderHeight, tile.bounds.right - 12, tile.bounds.bottom - 10});
        }
    }
}

bool MainWindow::HitTestTileIcon(POINT point, size_t& tileIndex, int& iconIndex) const {
    for (size_t index = 0; index < tileViews_.size(); ++index) {
        const int hit = tileViews_[index].grid.HitTest(point);
        if (hit >= 0) {
            tileIndex = index;
            iconIndex = hit;
            return true;
        }
    }
    return false;
}

bool MainWindow::HitTestTileHeader(POINT point, size_t& tileIndex) const {
    for (size_t index = 0; index < tileViews_.size(); ++index) {
        const RECT header = TileHeaderBounds(index);
        if (PtInRect(&header, point)) {
            tileIndex = index;
            return true;
        }
    }
    return false;
}

void MainWindow::OpenCurrentCategoryWidget() {
    const std::wstring categoryId = organizerConfig_.currentCategoryId;
    if (categoryId.empty()) {
        return;
    }

    for (auto it = widgetWindows_.begin(); it != widgetWindows_.end();) {
        if (*it == nullptr || !(*it)->IsOpen()) {
            it = widgetWindows_.erase(it);
            continue;
        }
        if ((*it)->IsForCategory(categoryId)) {
            (*it)->Show(SW_SHOWNOACTIVATE);
            return;
        }
        ++it;
    }

    const int spawnOffset = static_cast<int>(widgetWindows_.size()) * 36;
    auto widget = std::make_unique<WidgetWindow>(instance_, hwnd_, categoryId, spawnOffset);
    if (!widget->Create()) {
        return;
    }
    widget->Show(SW_SHOWNOACTIVATE);
    widgetWindows_.push_back(std::move(widget));
}

void MainWindow::OpenAllCategoryWidgets() {
    std::vector<std::wstring> categoryIds;
    if (!organizerConfig_.uncategorizedItemIds.empty() || organizerConfig_.categories.empty()) {
        categoryIds.push_back(kUncategorizedCategoryId);
    }
    for (const Category& category : organizerConfig_.categories) {
        categoryIds.push_back(category.id);
    }

    for (size_t index = 0; index < categoryIds.size(); ++index) {
        const std::wstring& categoryId = categoryIds[index];
        bool alreadyOpen = false;
        for (auto it = widgetWindows_.begin(); it != widgetWindows_.end();) {
            if (*it == nullptr || !(*it)->IsOpen()) {
                it = widgetWindows_.erase(it);
                continue;
            }
            if ((*it)->IsForCategory(categoryId)) {
                (*it)->SetVisible(true);
                alreadyOpen = true;
            }
            ++it;
        }
        if (alreadyOpen) {
            continue;
        }
        auto widget = std::make_unique<WidgetWindow>(
            instance_,
            hwnd_,
            categoryId,
            static_cast<int>(index) * 36);
        if (widget->Create()) {
            widget->Show(SW_SHOWNOACTIVATE);
            widgetWindows_.push_back(std::move(widget));
        }
    }
}

void MainWindow::MoveCurrentCategory(int delta) {
    if (organizerConfig_.currentCategoryId == kUncategorizedCategoryId || delta == 0) {
        return;
    }
    const auto current = std::find_if(
        organizerConfig_.categories.begin(),
        organizerConfig_.categories.end(),
        [&](const Category& category) { return category.id == organizerConfig_.currentCategoryId; });
    if (current == organizerConfig_.categories.end()) {
        return;
    }
    const auto currentIndex = static_cast<std::ptrdiff_t>(std::distance(organizerConfig_.categories.begin(), current));
    const auto targetIndex = currentIndex + static_cast<std::ptrdiff_t>(delta);
    if (targetIndex < 0 || targetIndex >= static_cast<std::ptrdiff_t>(organizerConfig_.categories.size())) {
        return;
    }
    std::iter_swap(
        organizerConfig_.categories.begin() + currentIndex,
        organizerConfig_.categories.begin() + targetIndex);
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetCategoryColor(const std::wstring& color) {
    Category* category = FindCategory(organizerConfig_.currentCategoryId);
    if (category == nullptr || color.empty()) {
        return;
    }
    category->color = color;
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetCategoryIcon(const std::wstring& icon) {
    Category* category = FindCategory(organizerConfig_.currentCategoryId);
    if (category == nullptr || icon.empty()) {
        return;
    }
    category->icon = icon;
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ExportCurrentCategory() {
    const Category* category = FindCategory(organizerConfig_.currentCategoryId);
    if (category == nullptr) {
        return;
    }
    wchar_t path[MAX_PATH] = L"Lattice-category.ini";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"Category (*.ini)\0*.ini\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.lpstrDefExt = L"ini";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&dialog) == FALSE) {
        return;
    }
    CategoryConfig exported;
    exported.id = category->id;
    exported.name = category->name;
    exported.itemIds = category->itemIds;
    exported.color = category->color;
    exported.icon = category->icon;
    exported.layout = category->layout;
    if (!configStore_.ExportCategoryConfig(exported, path)) {
        MessageDialog::Show(instance_, hwnd_, L"\u5206\u7c7b\u5bfc\u51fa\u5931\u8d25\uff0c\u8bf7\u9009\u62e9\u53ef\u5199\u5165\u7684\u4f4d\u7f6e\u3002", L"\u5206\u7c7b\u5bfc\u51fa", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::ImportCurrentCategory() {
    Category* category = FindCategory(organizerConfig_.currentCategoryId);
    if (category == nullptr) {
        return;
    }
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"Category (*.ini)\0*.ini\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = ARRAYSIZE(path);
    dialog.lpstrDefExt = L"ini";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog) == FALSE) {
        return;
    }
    if (MessageDialog::Show(instance_,
            hwnd_,
            L"\u5bfc\u5165\u5206\u7c7b\u4f1a\u8986\u76d6\u5f53\u524d\u5206\u7c7b\u7684\u540d\u79f0\u3001\u56fe\u6807\u548c\u9879\u76ee\u5173\u7cfb\uff0c\u662f\u5426\u7ee7\u7eed\uff1f",
            L"\u9700\u8981\u786e\u8ba4",
            MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    CategoryConfig imported;
    if (!configStore_.ImportCategoryConfig(path, imported)) {
        MessageDialog::Show(instance_, hwnd_, L"\u5206\u7c7b\u6587\u4ef6\u65e0\u6548\uff0c\u5f53\u524d\u914d\u7f6e\u672a\u6539\u53d8\u3002", L"\u5206\u7c7b\u5bfc\u5165", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring categoryId = category->id;
    CategoryStorageManager storageManager(configStore_, shortcutStore_);
    std::wstring storageError;
    if (!storageManager.Rename(categoryId, imported.name, storageError)) {
        MessageDialog::Show(instance_, hwnd_, storageError.c_str(), L"分类目录同步失败", MB_OK | MB_ICONWARNING);
        return;
    }
    LoadOrganizerConfig();
    category = FindCategory(categoryId);
    if (category == nullptr) {
        return;
    }
    category->name = imported.name;
    category->itemIds = imported.itemIds;
    category->color = imported.color;
    category->icon = imported.icon;
    category->layout = imported.layout;
    organizerConfig_.window = windowConfig_;
    SaveOrganizerConfig();
    LoadDesktopItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SaveLayoutProfile() {
    SaveWindowConfig();
    if (!configStore_.SaveLayoutProfile(windowConfig_)) {
        MessageDialog::Show(instance_, hwnd_, L"\u5e03\u5c40\u65b9\u6848\u4fdd\u5b58\u5931\u8d25\u3002", L"\u5e03\u5c40\u65b9\u6848", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::RestoreLayoutProfile() {
    WindowConfig profile;
    if (!configStore_.LoadLayoutProfile(profile)) {
        MessageDialog::Show(instance_, hwnd_, L"\u5c1a\u672a\u4fdd\u5b58\u5e03\u5c40\u65b9\u6848\u3002", L"\u5e03\u5c40\u65b9\u6848", MB_OK | MB_ICONINFORMATION);
        return;
    }
    windowConfig_ = profile;
    organizerConfig_.window = windowConfig_;
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(windowConfig_.opacity), LWA_ALPHA);
    SetWindowPos(hwnd_, nullptr, windowConfig_.x, windowConfig_.y, windowConfig_.width, windowConfig_.height, SWP_NOZORDER | SWP_NOACTIVATE);
    EnsureWindowVisible();
    RefreshCurrentItems();
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ResetLayout() {
    if (MessageDialog::Show(instance_,
            hwnd_,
            L"\u6062\u590d\u9ed8\u8ba4\u5e03\u5c40\u4f1a\u8986\u76d6\u5f53\u524d\u4e3b\u7a97\u53e3\u4f4d\u7f6e\u548c\u5c3a\u5bf8\uff0c\u662f\u5426\u7ee7\u7eed\uff1f",
            L"\u9700\u8981\u786e\u8ba4",
            MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    windowConfig_ = WindowConfig{};
    organizerConfig_.window = windowConfig_;
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(windowConfig_.opacity), LWA_ALPHA);
    SetWindowPos(hwnd_, nullptr, windowConfig_.x, windowConfig_.y, windowConfig_.width, windowConfig_.height, SWP_NOZORDER | SWP_NOACTIVATE);
    EnsureWindowVisible();
    RefreshCurrentItems();
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetViewMode(int viewMode) {
    const int nextViewMode = std::clamp(viewMode, 0, 1);
    const int previousViewMode = windowConfig_.viewMode;
    if (nextViewMode == 1 && previousViewMode != 1) {
        windowConfig_.viewMode = nextViewMode;
        OpenAllCategoryWidgets();
        ShowWindow(hwnd_, SW_HIDE);
    } else if (nextViewMode == 0 && previousViewMode == 1 && hasTileRestoreConfig_) {
        windowConfig_ = tileRestoreConfig_;
        windowConfig_.viewMode = nextViewMode;
        hasTileRestoreConfig_ = false;
        for (auto& widget : widgetWindows_) {
            if (widget != nullptr && widget->IsOpen()) {
                widget->SetVisible(false);
            }
        }
        SetWindowPos(
            hwnd_,
            nullptr,
            windowConfig_.x,
            windowConfig_.y,
            windowConfig_.width,
            windowConfig_.height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        windowConfig_.viewMode = nextViewMode;
        if (nextViewMode == 1) {
            OpenAllCategoryWidgets();
            ShowWindow(hwnd_, SW_HIDE);
        } else {
            for (auto& widget : widgetWindows_) {
                if (widget != nullptr && widget->IsOpen()) {
                    widget->SetVisible(false);
                }
            }
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }
    }
    organizerConfig_.window = windowConfig_;
    tileScrollOffset_ = 0;
    iconGrid_.SetBounds(GridBounds());
    RefreshCurrentItems();
    LayoutSearchEdit();
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetTabSide(int side) {
    windowConfig_.tabSide = std::clamp(side, 0, 3);
    organizerConfig_.window = windowConfig_;
    iconGrid_.SetBounds(GridBounds());
    RefreshTileViews();
    LayoutSearchEdit();
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RenameItemDisplayName(const std::wstring& itemId) {
    RegisteredItem* registered = nullptr;
    for (RegisteredItem& candidate : organizerConfig_.items) {
        if (candidate.id == itemId) {
            registered = &candidate;
            break;
        }
    }
    DesktopItem* item = FindItem(itemId);
    if (registered == nullptr || item == nullptr) {
        return;
    }
    const auto name = InputDialog::Prompt(
        instance_,
        hwnd_,
        L"\u91cd\u547d\u540d\u663e\u793a\u540d",
        L"\u663e\u793a\u540d",
        item->displayName);
    if (!name.has_value() || name->empty()) {
        return;
    }
    registered->displayName = *name;
    item->displayName = *name;
    SaveOrganizerConfig();
    RefreshCurrentItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RefreshIconCache() {
    iconCache_.Clear();
    for (auto& widget : widgetWindows_) {
        if (widget != nullptr && widget->IsOpen()) {
            widget->RefreshIconCache();
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ShowBackgroundMenu(POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    const bool canEditCategory = organizerConfig_.currentCategoryId != kUncategorizedCategoryId;
    AppendMenuW(menu, canEditCategory ? MF_STRING : MF_GRAYED, kExportCategoryCommand, L"\u5bfc\u51fa\u5f53\u524d\u5206\u7c7b");
    AppendMenuW(menu, canEditCategory ? MF_STRING : MF_GRAYED, kImportCategoryCommand, L"\u5bfc\u5165\u5f53\u524d\u5206\u7c7b");
    AppendMenuW(menu, MF_STRING, kNewCategoryCommand, L"新建分类...");
    const bool canEdit = organizerConfig_.currentCategoryId != kUncategorizedCategoryId;
    AppendMenuW(menu, canEdit ? MF_STRING : MF_GRAYED, kOpenWidgetCommand, L"打开当前分类为独立小组件");
    AppendMenuW(menu, canEdit ? MF_STRING : MF_GRAYED, kMoveCategoryUpCommand, L"当前分类上移");
    AppendMenuW(menu, canEdit ? MF_STRING : MF_GRAYED, kMoveCategoryDownCommand, L"当前分类下移");
    HMENU iconMenu = CreatePopupMenu();
    AppendMenuW(iconMenu, MF_STRING, kCategoryIconFolderCommand, L"\u6587\u4ef6\u5939");
    AppendMenuW(iconMenu, MF_STRING, kCategoryIconAppsCommand, L"\u5e94\u7528");
    AppendMenuW(iconMenu, MF_STRING, kCategoryIconWorkCommand, L"\u5de5\u4f5c");
    AppendMenuW(iconMenu, MF_STRING, kCategoryIconDocsCommand, L"\u6587\u6863");
    AppendMenuW(iconMenu, MF_STRING, kCategoryIconMediaCommand, L"\u5a92\u4f53");
    AppendMenuW(menu, canEdit ? MF_POPUP : MF_POPUP | MF_GRAYED, reinterpret_cast<UINT_PTR>(iconMenu), L"\u5206\u7c7b\u56fe\u6807");
    HMENU viewMenu = CreatePopupMenu();
    AppendMenuW(viewMenu, MF_STRING | (windowConfig_.viewMode == 0 ? MF_CHECKED : 0), kViewTabsCommand, L"\u5206\u7c7b\u6807\u7b7e");
    AppendMenuW(viewMenu, MF_STRING | (windowConfig_.viewMode == 1 ? MF_CHECKED : 0), kViewTileCommand, L"\u5206\u7c7b\u5e73\u94fa");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"\u4e3b\u753b\u5e03\u5c40");
    HMENU tabMenu = CreatePopupMenu();
    AppendMenuW(tabMenu, MF_STRING | (windowConfig_.tabSide == 0 ? MF_CHECKED : 0), kTabTopCommand, L"\u9876\u90e8");
    AppendMenuW(tabMenu, MF_STRING | (windowConfig_.tabSide == 1 ? MF_CHECKED : 0), kTabBottomCommand, L"\u5e95\u90e8");
    AppendMenuW(tabMenu, MF_STRING | (windowConfig_.tabSide == 2 ? MF_CHECKED : 0), kTabLeftCommand, L"\u5de6\u4fa7");
    AppendMenuW(tabMenu, MF_STRING | (windowConfig_.tabSide == 3 ? MF_CHECKED : 0), kTabRightCommand, L"\u53f3\u4fa7");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tabMenu), L"\u5206\u7c7b\u6807\u7b7e\u65b9\u5411");
    HMENU colorMenu = CreatePopupMenu();
    AppendMenuW(colorMenu, MF_STRING, kCategoryColorBlueCommand, L"蓝色");
    AppendMenuW(colorMenu, MF_STRING, kCategoryColorMintCommand, L"薄荷绿");
    AppendMenuW(colorMenu, MF_STRING, kCategoryColorAmberCommand, L"琥珀色");
    AppendMenuW(colorMenu, MF_STRING, kCategoryColorVioletCommand, L"紫色");
    AppendMenuW(menu, canEdit ? MF_POPUP : MF_POPUP | MF_GRAYED, reinterpret_cast<UINT_PTR>(colorMenu), L"分类强调色");
    AppendMenuW(menu, canEdit ? MF_STRING : MF_GRAYED, kRenameCategoryCommand, L"重命名当前分类...");
    AppendMenuW(menu, canEdit ? MF_STRING : MF_GRAYED, kDeleteCategoryCommand, L"删除当前分类");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kImportDesktopCommand, L"\u5bfc\u5165\u672a\u6536\u7eb3\u684c\u9762\u9879");
    AppendMenuW(menu, MF_STRING, kRefreshDesktopCommand, L"刷新桌面项目");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kToggleCollapseCommand, windowConfig_.collapsed ? L"展开格子" : L"折叠格子");
    AppendMenuW(menu, MF_STRING, kToggleLockCommand, windowConfig_.locked ? L"解除锁定" : L"锁定位置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExportConfigCommand, L"导出配置...");
    AppendMenuW(menu, MF_STRING, kImportConfigCommand, L"导入配置...");
    AppendMenuW(menu, MF_STRING, kSaveLayoutProfileCommand, L"\u4fdd\u5b58\u5e03\u5c40\u65b9\u6848");
    AppendMenuW(menu, MF_STRING, kRestoreLayoutProfileCommand, L"\u6062\u590d\u5e03\u5c40\u65b9\u6848");
    AppendMenuW(menu, MF_STRING, kResetLayoutCommand, L"\u6062\u590d\u9ed8\u8ba4\u5e03\u5c40");
    HMENU sizeMenu = CreatePopupMenu();
    AppendMenuW(sizeMenu, MF_STRING | (windowConfig_.iconSize <= 36 ? MF_CHECKED : 0), kIconSmallCommand, L"小图标");
    AppendMenuW(sizeMenu, MF_STRING | (windowConfig_.iconSize > 36 && windowConfig_.iconSize < 64 ? MF_CHECKED : 0), kIconMediumCommand, L"中图标");
    AppendMenuW(sizeMenu, MF_STRING | (windowConfig_.iconSize >= 64 ? MF_CHECKED : 0), kIconLargeCommand, L"大图标");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizeMenu), L"图标大小");
    HMENU densityMenu = CreatePopupMenu();
    AppendMenuW(densityMenu, MF_STRING | (windowConfig_.density == 0 ? MF_CHECKED : 0), kDensityCompactCommand, L"紧凑网格");
    AppendMenuW(densityMenu, MF_STRING | (windowConfig_.density == 1 ? MF_CHECKED : 0), kDensityStandardCommand, L"标准网格");
    AppendMenuW(densityMenu, MF_STRING | (windowConfig_.density == 2 ? MF_CHECKED : 0), kDensitySpaciousCommand, L"宽松网格");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(densityMenu), L"网格密度");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"退出");
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowIconMenu(POINT screenPoint, int iconIndex) {
    const DesktopItem* item = iconGrid_.ItemAt(static_cast<size_t>(iconIndex));
    if (item == nullptr) {
        return;
    }
    ShowItemMenu(screenPoint, item->id);
}

void MainWindow::ShowItemMenu(POINT screenPoint, const std::wstring& itemId) {
    DesktopItem* item = FindItem(itemId);
    if (item == nullptr) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kRunAsAdminCommand, L"\u4ee5\u7ba1\u7406\u5458\u8fd0\u884c");
    AppendMenuW(menu, MF_STRING, kRenameItemCommand, L"\u91cd\u547d\u540d\u663e\u793a\u540d");
    AppendMenuW(menu, MF_STRING, kRefreshIconCommand, L"\u5237\u65b0\u56fe\u6807");
    AppendMenuW(menu, MF_STRING, kOpenItemCommand, L"打开");
    AppendMenuW(menu, MF_STRING, kShowItemCommand, L"打开所在位置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU moveMenu = CreatePopupMenu();
    for (size_t index = 0; index < organizerConfig_.categories.size(); ++index) {
        AppendMenuW(moveMenu, MF_STRING, kMoveItemBaseCommand + static_cast<UINT>(index), organizerConfig_.categories[index].name.c_str());
    }
    if (organizerConfig_.categories.empty()) {
        AppendMenuW(moveMenu, MF_GRAYED, 0, L"暂无分类");
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveMenu), L"移动到分类");

    AppendMenuW(menu, MF_STRING, kRemoveItemCommand, L"移出格子");

    const int command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    if (command == kOpenItemCommand) {
        launcher_.OpenPath(item->path);
    } else if (command == kShowItemCommand) {
        launcher_.ShowInExplorer(item->path);
    } else if (command == kRunAsAdminCommand) {
        if (MessageDialog::Show(instance_,
                hwnd_,
                L"\u5c06\u4ee5\u7ba1\u7406\u5458\u6743\u9650\u8fd0\u884c\u8be5\u9879\u76ee\uff1f",
                L"\u9700\u8981\u786e\u8ba4",
                MB_YESNO | MB_ICONWARNING) == IDYES) {
            launcher_.RunAsAdministrator(item->path);
        }
    } else if (command == kRenameItemCommand) {
        RenameItemDisplayName(item->id);
    } else if (command == kRefreshIconCommand) {
        RefreshIconCache();
    } else if (command == kRemoveItemCommand) {
        RemoveItemFromCurrentCategory(item->id);
    } else if (command >= kMoveItemBaseCommand &&
               command < kMoveItemBaseCommand + static_cast<int>(organizerConfig_.categories.size())) {
        const size_t categoryIndex = static_cast<size_t>(command - kMoveItemBaseCommand);
        MoveItemToCategory(item->id, organizerConfig_.categories[categoryIndex].id);
    }

    DestroyMenu(menu);
}

void MainWindow::ShowTileMenu(POINT screenPoint, size_t tileIndex) {
    if (tileIndex >= tileViews_.size()) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(
        menu,
        MF_STRING,
        kTileToggleCollapseCommand,
        tileViews_[tileIndex].collapsed ? L"\u5c55\u5f00\u5206\u7c7b\u5361\u7247" : L"\u6298\u53e0\u5206\u7c7b\u5361\u7247");
    AppendMenuW(menu, MF_STRING, kTileOpenWidgetCommand, L"\u6253\u5f00\u4e3a\u72ec\u7acb\u5c0f\u7ec4\u4ef6");
    const int command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (command == kTileToggleCollapseCommand) {
        const std::wstring categoryId = tileViews_[tileIndex].categoryId;
        const bool collapsed = !tileViews_[tileIndex].collapsed;
        if (Category* category = FindCategory(categoryId); category != nullptr) {
            category->tileCollapsed = collapsed;
            SaveOrganizerConfig();
        } else {
            tileCollapsed_[categoryId] = collapsed;
        }
        RefreshTileViews();
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (command == kTileOpenWidgetCommand) {
        organizerConfig_.currentCategoryId = tileViews_[tileIndex].categoryId;
        OpenCurrentCategoryWidget();
    }
}

void MainWindow::CreateCategory() {
    const std::wstring defaultName = L"分类 " + std::to_wstring(organizerConfig_.categories.size() + 1);
    const auto name = InputDialog::Prompt(instance_, hwnd_, L"新建格子", L"格子名称", defaultName);
    if (!name.has_value()) {
        return;
    }
    Category category;
    category.id = GenerateCategoryId();
    CategoryStorageManager storageManager(configStore_, shortcutStore_);
    std::wstring storageError;
    if (!storageManager.CanUseName(category.id, *name, storageError)) {
        MessageDialog::Show(instance_, hwnd_, storageError.c_str(), L"无法创建格子", MB_OK | MB_ICONWARNING);
        return;
    }
    category.name = *name;
    category.storageFolder = *name;
    category.color = DeskGoPaletteColor(organizerConfig_.categories.size());
    category.layout.width = 390;
    category.layout.height = 489;
    category.layout.normalHeight = 489;
    category.layout.iconSize = 48;
    category.layout.density = 0;
    category.layout.viewMode = 1;
    category.layout.opacity = 230;
    category.layout.titleOpacity = 210;
    category.layout.showBorder = true;

    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        constexpr int margin = 24;
        constexpr int topOffset = 96;
        constexpr int gap = 16;
        const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        const int columns = std::max(1, (workWidth - margin * 2 + gap) / (category.layout.width + gap));
        const size_t slot = organizerConfig_.categories.size() +
                            (organizerConfig_.uncategorizedItemIds.empty() ? 0U : 1U);
        const int column = static_cast<int>(slot % static_cast<size_t>(columns));
        const int row = static_cast<int>(slot / static_cast<size_t>(columns));
        category.layout.x = monitorInfo.rcWork.right - margin - category.layout.width -
                            column * (category.layout.width + gap);
        category.layout.y = monitorInfo.rcWork.top + topOffset + row * (category.layout.height + gap);
        if (category.layout.y + category.layout.height > monitorInfo.rcWork.bottom - margin) {
            category.layout.y = monitorInfo.rcWork.top + topOffset + static_cast<int>(slot) * 36;
            category.layout.x = monitorInfo.rcWork.right - margin - category.layout.width - static_cast<int>(slot) * 36;
        }
        category.layout.monitorId = monitorInfo.szDevice;
    }
    organizerConfig_.currentCategoryId = category.id;
    organizerConfig_.categories.push_back(std::move(category));
    RefreshCurrentItems();
    if (SaveOrganizerConfig()) {
        const std::wstring categoryId = organizerConfig_.currentCategoryId;
        if (!storageManager.Rename(categoryId, *name, storageError)) {
            MessageDialog::Show(instance_, hwnd_, storageError.c_str(), L"格子文件夹同步失败", MB_OK | MB_ICONWARNING);
        }
        LoadOrganizerConfig();
        OpenCurrentCategoryWidget();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RenameCurrentCategory() {
    Category* category = FindCategory(organizerConfig_.currentCategoryId);
    if (category == nullptr) {
        return;
    }
    const auto name = InputDialog::Prompt(instance_, hwnd_, L"重命名格子", L"格子名称", category->name);
    if (!name.has_value()) {
        return;
    }
    CategoryStorageManager storageManager(configStore_, shortcutStore_);
    std::wstring errorMessage;
    if (!storageManager.Rename(category->id, *name, errorMessage)) {
        MessageDialog::Show(instance_, hwnd_, errorMessage.c_str(), L"重命名格子失败", MB_OK | MB_ICONWARNING);
        return;
    }
    LoadOrganizerConfig();
    LoadDesktopItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteCurrentCategory() {
    if (organizerConfig_.currentCategoryId == kUncategorizedCategoryId) {
        return;
    }
    if (MessageDialog::Show(instance_,
            hwnd_,
            L"删除分类会把其中的桌面项目移回原位置，不会删除项目。确定继续？",
            L"删除分类",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    const std::wstring id = organizerConfig_.currentCategoryId;
    const Category* category = FindCategory(id);
    const std::vector<std::wstring> itemIds = category == nullptr ? std::vector<std::wstring>{} : category->itemIds;
    for (const std::wstring& itemId : itemIds) {
        if (!MoveItemOut(itemId, false)) {
            MessageDialog::Show(instance_,
                hwnd_,
                L"至少一个桌面项目无法安全移回原位置。分类会保留，已成功移出的项目不会重复显示。",
                L"未能删除分类",
                MB_OK | MB_ICONERROR);
            return;
        }
    }
    CategoryStorageManager storageManager(configStore_, shortcutStore_);
    std::wstring storageError;
    if (!storageManager.RemoveEmpty(id, storageError)) {
        MessageDialog::Show(instance_, hwnd_, storageError.c_str(), L"未能删除分类", MB_OK | MB_ICONWARNING);
        return;
    }
    for (auto& widget : widgetWindows_) {
        if (widget != nullptr && widget->IsForCategory(id)) {
            widget->Close();
        }
    }
    widgetWindows_.erase(
        std::remove_if(widgetWindows_.begin(), widgetWindows_.end(), [&](const auto& widget) {
            return widget == nullptr || widget->IsForCategory(id);
        }),
        widgetWindows_.end());
    organizerConfig_.categories.erase(
        std::remove_if(organizerConfig_.categories.begin(), organizerConfig_.categories.end(), [&](const Category& category) {
            return category.id == id;
        }),
        organizerConfig_.categories.end());
    organizerConfig_.currentCategoryId = kUncategorizedCategoryId;
    RefreshCurrentItems();
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleCollapsed() {
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    windowConfig_.collapsed = !windowConfig_.collapsed;
    windowConfig_.x = rect.left;
    windowConfig_.y = rect.top;
    windowConfig_.width = rect.right - rect.left;
    if (windowConfig_.collapsed) {
        windowConfig_.normalHeight = rect.bottom - rect.top;
        windowConfig_.height = kTitleHeight + kTabsHeight + 12;
    } else {
        windowConfig_.height = std::max(windowConfig_.normalHeight, 320);
    }
    SetWindowPos(hwnd_, nullptr, windowConfig_.x, windowConfig_.y, windowConfig_.width, windowConfig_.height, SWP_NOZORDER);
    organizerConfig_.window = windowConfig_;
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleLocked() {
    windowConfig_.locked = !windowConfig_.locked;
    organizerConfig_.window = windowConfig_;
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleAllLocked() {
    const bool locked = !windowConfig_.locked;
    windowConfig_.locked = locked;
    organizerConfig_.window = windowConfig_;
    SaveOrganizerConfig();
    for (auto& widget : widgetWindows_) {
        if (widget != nullptr && widget->IsOpen()) {
            widget->SetLocked(locked);
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetIconSize(int iconSize) {
    windowConfig_.iconSize = std::clamp(iconSize, 32, 72);
    iconGrid_.SetIconSize(windowConfig_.iconSize);
    iconGrid_.SetBounds(GridBounds());
    RefreshTileViews();
    organizerConfig_.window = windowConfig_;
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetDensity(int density) {
    windowConfig_.density = std::clamp(density, 0, 2);
    iconGrid_.SetDensity(windowConfig_.density);
    RefreshTileViews();
    organizerConfig_.window = windowConfig_;
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ImportUnassignedDesktopItems() {
    DesktopScanner scanner;
    items_ = scanner.Scan();
    for (const RegisteredItem& registeredItem : organizerConfig_.items) {
        scanner.MergeRegisteredItem(
            items_,
            registeredItem.id,
            registeredItem.path,
            registeredItem.displayName);
    }

    std::vector<std::wstring> paths;
    for (const DesktopItem& item : items_) {
        if (!IsItemAssigned(item.id)) {
            paths.push_back(item.path);
        }
    }
    bool changed = false;
    for (const std::wstring& path : paths) {
        if (shortcutStore_.IsSupportedShortcut(path)) {
            changed = ImportPathToCategory(path, organizerConfig_.currentCategoryId, false) || changed;
        }
    }
    if (changed) {
        LoadDesktopItems();
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
}

void MainWindow::ReorderCurrentCategoryItem(size_t fromIndex, size_t toIndex) {
    if (!searchQuery_.empty() || searchScope_ != 0) {
        return;
    }
    if (fromIndex >= currentItems_.size() || toIndex >= currentItems_.size()) {
        return;
    }
    if (fromIndex == toIndex) {
        return;
    }
    windowConfig_.autoArrange = false;
    windowConfig_.sortMode = 0;
    if (organizerConfig_.currentCategoryId == kUncategorizedCategoryId) {
        organizerConfig_.window.autoArrange = false;
        organizerConfig_.window.sortMode = 0;
    } else if (Category* category = FindCategory(organizerConfig_.currentCategoryId); category != nullptr) {
        category->layout.autoArrange = false;
        category->layout.sortMode = 0;
    }

    std::vector<std::wstring>* itemIds = nullptr;
    if (organizerConfig_.currentCategoryId == kUncategorizedCategoryId) {
        itemIds = &organizerConfig_.uncategorizedItemIds;
    } else {
        Category* category = FindCategory(organizerConfig_.currentCategoryId);
        if (category != nullptr) {
            itemIds = &category->itemIds;
        }
    }
    if (itemIds == nullptr) {
        return;
    }

    const std::wstring movingId = currentItems_[fromIndex].id;
    const std::wstring targetId = currentItems_[toIndex].id;
    auto fromIt = std::find(itemIds->begin(), itemIds->end(), movingId);
    auto targetIt = std::find(itemIds->begin(), itemIds->end(), targetId);
    if (fromIt == itemIds->end() || targetIt == itemIds->end()) {
        return;
    }

    const size_t targetPosition = static_cast<size_t>(std::distance(itemIds->begin(), targetIt));
    itemIds->erase(fromIt);
    const size_t adjustedTarget = std::min(targetPosition, itemIds->size());
    itemIds->insert(itemIds->begin() + static_cast<std::ptrdiff_t>(adjustedTarget), movingId);
    RefreshCurrentItems();
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void MainWindow::ReorderItemInCategory(
    const std::wstring& categoryId,
    const std::wstring& fromItemId,
    const std::wstring& toItemId) {
    std::vector<std::wstring>* itemIds = nullptr;
    if (categoryId == kUncategorizedCategoryId) {
        itemIds = &organizerConfig_.uncategorizedItemIds;
        windowConfig_.autoArrange = false;
        windowConfig_.sortMode = 0;
        organizerConfig_.window.autoArrange = false;
        organizerConfig_.window.sortMode = 0;
    } else {
        Category* category = FindCategory(categoryId);
        if (category != nullptr) {
            itemIds = &category->itemIds;
            category->layout.autoArrange = false;
            category->layout.sortMode = 0;
        }
    }
    if (itemIds == nullptr || fromItemId.empty() || toItemId.empty() || fromItemId == toItemId) {
        return;
    }
    const auto fromIt = std::find(itemIds->begin(), itemIds->end(), fromItemId);
    const auto targetIt = std::find(itemIds->begin(), itemIds->end(), toItemId);
    if (fromIt == itemIds->end() || targetIt == itemIds->end()) {
        return;
    }
    const size_t targetPosition = static_cast<size_t>(std::distance(itemIds->begin(), targetIt));
    itemIds->erase(fromIt);
    itemIds->insert(
        itemIds->begin() + static_cast<std::ptrdiff_t>(std::min(targetPosition, itemIds->size())),
        fromItemId);
    RefreshCurrentItems();
    SaveOrganizerConfig();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveItemToCategory(const std::wstring& itemId, const std::wstring& categoryId) {
    DesktopItem* item = FindItem(itemId);
    if (item == nullptr || categoryId.empty()) {
        return;
    }
    if (categoryId != kUncategorizedCategoryId && FindCategory(categoryId) == nullptr) {
        return;
    }
    const std::wstring sourcePath = item->path;
    const std::wstring displayName = item->displayName;
    DesktopScanner scanner;
    const std::wstring sourceVisibleId = itemId;
    const std::wstring sourceDerivedId =
        scanner.CreateItemFromPath(sourcePath, false).id;
    std::wstring managedItemId;
    const auto existingByPath = std::find_if(
        organizerConfig_.items.begin(),
        organizerConfig_.items.end(),
        [&](const RegisteredItem& value) {
            return CompareStringOrdinal(
                       value.path.c_str(),
                       -1,
                       sourcePath.c_str(),
                       -1,
                       TRUE) == CSTR_EQUAL;
        });
    if (existingByPath != organizerConfig_.items.end()) {
        managedItemId = existingByPath->id;
    } else if (!scanner.TryCreateManagedItemId(
                   [&](const std::wstring& candidate) {
                       return std::any_of(
                           organizerConfig_.items.begin(),
                           organizerConfig_.items.end(),
                           [&](const RegisteredItem& value) {
                               return value.id == candidate;
                           });
                   },
                   managedItemId)) {
        MessageDialog::Show(
            instance_,
            hwnd_,
            L"无法为该项目创建安全的唯一标识，未移动任何内容。",
            L"移动桌面项目失败",
            MB_OK | MB_ICONERROR);
        return;
    }
    const OrganizerConfig originalConfig = organizerConfig_;
    std::wstring destinationPath;
    std::wstring errorMessage;
    const auto persistCategoryMove = [&](const std::wstring& storedPath) {
            auto registered = std::find_if(organizerConfig_.items.begin(), organizerConfig_.items.end(), [&](const RegisteredItem& value) {
                return value.id == managedItemId;
            });
            if (registered == organizerConfig_.items.end()) {
                organizerConfig_.items.push_back(RegisteredItem{managedItemId, storedPath, displayName});
            } else {
                registered->path = storedPath;
            }
            std::vector<std::wstring> removalIds{managedItemId};
            const auto appendTransientId = [&](const std::wstring& candidate) {
                if (candidate.empty() || candidate == managedItemId) {
                    return;
                }
                const bool hasRegisteredOwner = std::any_of(
                    organizerConfig_.items.begin(),
                    organizerConfig_.items.end(),
                    [&](const RegisteredItem& value) {
                        return value.id == candidate;
                    });
                if (!hasRegisteredOwner &&
                    std::find(removalIds.begin(), removalIds.end(), candidate) == removalIds.end()) {
                    removalIds.push_back(candidate);
                }
            };
            appendTransientId(sourceDerivedId);
            appendTransientId(sourceVisibleId);
            const auto eraseRemovalIds = [&](std::vector<std::wstring>& itemIds) {
                itemIds.erase(
                    std::remove_if(
                        itemIds.begin(),
                        itemIds.end(),
                        [&](const std::wstring& value) {
                            return std::find(removalIds.begin(), removalIds.end(), value) != removalIds.end();
                        }),
                    itemIds.end());
            };
            eraseRemovalIds(organizerConfig_.uncategorizedItemIds);
            for (Category& category : organizerConfig_.categories) {
                eraseRemovalIds(category.itemIds);
            }
            if (categoryId == kUncategorizedCategoryId) {
                organizerConfig_.uncategorizedItemIds.push_back(managedItemId);
                organizerConfig_.currentCategoryId = kUncategorizedCategoryId;
            } else {
                Category* target = FindCategory(categoryId);
                if (target == nullptr) {
                    organizerConfig_ = originalConfig;
                    return false;
                }
                target->itemIds.push_back(managedItemId);
                organizerConfig_.currentCategoryId = target->id;
            }
            if (!SaveOrganizerConfig()) {
                organizerConfig_ = originalConfig;
                return false;
            }
            return true;
        };
    const bool requiresPhysicalMove =
        shortcutStore_.RequiresManagedStorage(sourcePath);
    bool moved = false;
    if (requiresPhysicalMove) {
        moved = shortcutStore_.MoveIntoCategory(
            managedItemId,
            sourcePath,
            StorageFolderForCategory(categoryId),
            persistCategoryMove,
            destinationPath,
            errorMessage);
    } else {
        destinationPath = sourcePath;
        moved = persistCategoryMove(destinationPath);
        if (!moved) {
            errorMessage = L"无法保存该文件或文件夹的分类，原件未发生改变。";
        }
    }
    if (!moved) {
        organizerConfig_ = originalConfig;
        MessageDialog::Show(instance_, hwnd_, errorMessage.c_str(), L"移动桌面项目失败", MB_OK | MB_ICONERROR);
        return;
    }
    LoadDesktopItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RemoveItemFromCurrentCategory(const std::wstring& itemId) {
    MoveItemOut(itemId);
}

bool MainWindow::MoveItemOut(
    const std::wstring& itemId,
    bool showError,
    const POINT* dropScreenPoint,
    std::uint64_t dragGhostGeneration) {
    DesktopItem* item = FindItem(itemId);
    if (item == nullptr) {
        return false;
    }
    const std::wstring sourcePath = item->path;
    const OrganizerConfig originalConfig = organizerConfig_;
    const auto registeredBeforeRemoval = std::find_if(originalConfig.items.begin(), originalConfig.items.end(), [&](const RegisteredItem& value) {
        return value.id == itemId;
    });
    const RegisteredItem placement = registeredBeforeRemoval == originalConfig.items.end() ? RegisteredItem{} : *registeredBeforeRemoval;
    const auto persistRemoval = [&]() {
        organizerConfig_.uncategorizedItemIds.erase(
            std::remove(organizerConfig_.uncategorizedItemIds.begin(), organizerConfig_.uncategorizedItemIds.end(), itemId),
            organizerConfig_.uncategorizedItemIds.end());
        for (Category& category : organizerConfig_.categories) {
            category.itemIds.erase(
                std::remove(category.itemIds.begin(), category.itemIds.end(), itemId),
                category.itemIds.end());
        }
        organizerConfig_.items.erase(
            std::remove_if(organizerConfig_.items.begin(), organizerConfig_.items.end(), [&](const RegisteredItem& value) {
                return value.id == itemId;
            }),
            organizerConfig_.items.end());
        if (!SaveOrganizerConfig()) {
            organizerConfig_ = originalConfig;
            return false;
        }
        return true;
    };

    bool moved = false;
    std::wstring desktopPath = sourcePath;
    std::wstring errorMessage;
    if (!shortcutStore_.IsManagedPath(sourcePath)) {
        moved = persistRemoval();
        if (!moved) {
            errorMessage = L"项目原件未发生改变，但无法保存移出格子的配置。";
        }
    } else {
        std::wstring destinationPath;
        const bool hasValidOriginalDesktopPath =
            !placement.originalDesktopPath.empty() &&
            shortcutStore_.IsDesktopPath(placement.originalDesktopPath);
        const std::wstring targetPath = hasValidOriginalDesktopPath
            ? placement.originalDesktopPath
            : JoinPath(shortcutStore_.DesktopPath(), FileNameFromPath(sourcePath));
        moved = shortcutStore_.MoveToOriginalDesktop(
            itemId,
            sourcePath,
            targetPath,
            [&](const std::wstring&) { return persistRemoval(); },
            destinationPath,
            errorMessage);
        if (moved) {
            desktopPath = destinationPath;
        }
        if (moved && placement.hasDesktopPosition && dropScreenPoint == nullptr) {
            DesktopLayout desktopLayout;
            std::wstring restoreError;
            if (!desktopLayout.RestorePositions(
                    {DesktopPosition{destinationPath, POINT{placement.desktopX, placement.desktopY}}},
                    restoreError) && showError) {
                MessageDialog::Show(instance_, hwnd_, restoreError.c_str(), L"桌面坐标恢复", MB_OK | MB_ICONWARNING);
            }
        }
    }
    if (!moved) {
        organizerConfig_ = originalConfig;
        DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
        if (showError) {
            MessageDialog::Show(instance_, hwnd_, errorMessage.c_str(), L"移出格子失败", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    const bool canPlaceOnDesktop = shortcutStore_.IsDesktopPath(desktopPath);
    if (dropScreenPoint != nullptr && canPlaceOnDesktop) {
        DesktopPlacementRequest request;
        request.path = desktopPath;
        request.screenPoint = *dropScreenPoint;
        request.dragGhostGeneration = dragGhostGeneration;
        request.showError = showError;
        request.sourceWindow = hwnd_;
        if (!QueueDesktopPlacement(request)) {
            DesktopLayout desktopLayout;
            POINT restoredPoint{};
            std::wstring restoreError;
            const bool restored = desktopLayout.RestoreScreenPosition(
                desktopPath,
                *dropScreenPoint,
                restoredPoint,
                restoreError);
            DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
            if (restored) {
                SaveDesktopPlacement(
                    desktopPath,
                    restoredPoint,
                    showError,
                    hwnd_,
                    true);
            } else if (showError) {
                MessageDialog::Show(
                    instance_,
                    hwnd_,
                    restoreError.c_str(),
                    L"桌面坐标恢复",
                    MB_OK | MB_ICONWARNING);
            }
        }
    } else if (dropScreenPoint != nullptr) {
        DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
    }
    if (dropScreenPoint != nullptr) {
        items_.erase(
            std::remove_if(items_.begin(), items_.end(), [&](const DesktopItem& value) {
                return value.id == itemId;
            }),
            items_.end());
        RefreshCurrentItems();
    } else {
        LoadDesktopItems();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool MainWindow::ImportPathToCategory(
    const std::wstring& path,
    const std::wstring& categoryId,
    bool showError) {
    if (path.empty() || categoryId.empty() ||
        (categoryId != kUncategorizedCategoryId && FindCategory(categoryId) == nullptr)) {
        return false;
    }
    DesktopScanner scanner;
    DesktopItem item = scanner.CreateItemFromPath(path, false);
    const std::wstring sourceDerivedId = item.id;
    std::wstring sourceVisibleId = sourceDerivedId;
    const auto visibleItem = std::find_if(
        items_.begin(),
        items_.end(),
        [&](const DesktopItem& value) {
            return CompareStringOrdinal(
                       value.path.c_str(),
                       -1,
                       item.path.c_str(),
                       -1,
                       TRUE) == CSTR_EQUAL;
        });
    if (visibleItem != items_.end()) {
        sourceVisibleId = visibleItem->id;
        item.displayName = visibleItem->displayName;
        item.targetPath = visibleItem->targetPath;
        item.arguments = visibleItem->arguments;
        item.workingDirectory = visibleItem->workingDirectory;
        item.kind = visibleItem->kind;
        item.missing = visibleItem->missing;
    }
    if (!shortcutStore_.IsSupportedDesktopItem(item.path)) {
        if (showError) {
            MessageDialog::Show(instance_,
                hwnd_,
                L"该文件、文件夹或快捷方式当前不可访问，未移动任何内容。",
                L"无法收纳该项目",
                MB_OK | MB_ICONINFORMATION);
        }
        return false;
    }
    const auto existing = std::find_if(organizerConfig_.items.begin(), organizerConfig_.items.end(), [&](const RegisteredItem& value) {
        return CompareStringOrdinal(value.path.c_str(), -1, item.path.c_str(), -1, TRUE) == CSTR_EQUAL;
    });
    if (existing != organizerConfig_.items.end()) {
        item.id = existing->id;
        if (!existing->displayName.empty()) {
            item.displayName = existing->displayName;
        }
    } else if (!scanner.TryCreateManagedItemId(
                   [&](const std::wstring& candidate) {
                       return std::any_of(
                           organizerConfig_.items.begin(),
                           organizerConfig_.items.end(),
                           [&](const RegisteredItem& value) {
                               return value.id == candidate;
                           });
                   },
                   item.id)) {
        if (showError) {
            MessageDialog::Show(
                instance_,
                hwnd_,
                L"无法为该项目创建安全的唯一标识，未移动任何内容。",
                L"无法收纳该项目",
                MB_OK | MB_ICONERROR);
        }
        return false;
    }

    std::wstring originalDesktopPath;
    const bool physicallyManagedShortcut =
        shortcutStore_.RequiresManagedStorage(item.path);
    POINT originalDesktopPoint{};
    bool hasOriginalDesktopPoint = false;
    if (shortcutStore_.IsDesktopPath(item.path)) {
        originalDesktopPath = item.path;
        DesktopLayout desktopLayout;
        std::wstring captureError;
        if (!desktopLayout.CapturePosition(item.path, originalDesktopPoint, captureError)) {
            if (physicallyManagedShortcut && showError) {
                MessageDialog::Show(instance_, hwnd_, captureError.c_str(), L"桌面布局保护", MB_OK | MB_ICONWARNING);
            }
            if (physicallyManagedShortcut) {
                return false;
            }
        } else {
            hasOriginalDesktopPoint = true;
        }
    }

    const OrganizerConfig originalConfig = organizerConfig_;
    std::wstring destinationPath;
    std::wstring errorMessage;
    const auto persistCollectedItem = [&](const std::wstring& storedPath) {
            auto registered = std::find_if(organizerConfig_.items.begin(), organizerConfig_.items.end(), [&](const RegisteredItem& value) {
                return value.id == item.id;
            });
            if (registered == organizerConfig_.items.end()) {
                organizerConfig_.items.push_back(RegisteredItem{item.id, storedPath, item.displayName});
                registered = std::prev(organizerConfig_.items.end());
            } else {
                registered->path = storedPath;
            }
            registered->originalDesktopPath = originalDesktopPath;
            registered->desktopX = originalDesktopPoint.x;
            registered->desktopY = originalDesktopPoint.y;
            registered->hasDesktopPosition = hasOriginalDesktopPoint;
            std::vector<std::wstring> removalIds{item.id};
            const auto appendTransientId = [&](const std::wstring& candidate) {
                if (candidate.empty() || candidate == item.id) {
                    return;
                }
                const bool hasRegisteredOwner = std::any_of(
                    organizerConfig_.items.begin(),
                    organizerConfig_.items.end(),
                    [&](const RegisteredItem& value) {
                        return value.id == candidate;
                    });
                if (!hasRegisteredOwner &&
                    std::find(removalIds.begin(), removalIds.end(), candidate) == removalIds.end()) {
                    removalIds.push_back(candidate);
                }
            };
            appendTransientId(sourceDerivedId);
            appendTransientId(sourceVisibleId);
            const auto eraseRemovalIds = [&](std::vector<std::wstring>& itemIds) {
                itemIds.erase(
                    std::remove_if(
                        itemIds.begin(),
                        itemIds.end(),
                        [&](const std::wstring& value) {
                            return std::find(removalIds.begin(), removalIds.end(), value) != removalIds.end();
                        }),
                    itemIds.end());
            };
            eraseRemovalIds(organizerConfig_.uncategorizedItemIds);
            for (Category& category : organizerConfig_.categories) {
                eraseRemovalIds(category.itemIds);
            }
            if (categoryId == kUncategorizedCategoryId) {
                organizerConfig_.uncategorizedItemIds.push_back(item.id);
            } else {
                Category* target = FindCategory(categoryId);
                if (target == nullptr) {
                    organizerConfig_ = originalConfig;
                    return false;
                }
                target->itemIds.push_back(item.id);
            }
            organizerConfig_.currentCategoryId = categoryId;
            if (!SaveOrganizerConfig()) {
                organizerConfig_ = originalConfig;
                return false;
            }
            return true;
        };
    bool moved = false;
    if (physicallyManagedShortcut) {
        moved = shortcutStore_.MoveIntoCategory(
            item.id,
            item.path,
            StorageFolderForCategory(categoryId),
            persistCollectedItem,
            destinationPath,
            errorMessage);
    } else {
        destinationPath = item.path;
        moved = persistCollectedItem(destinationPath);
        if (!moved) {
            errorMessage = L"无法保存该文件或文件夹的收纳配置，原件未发生改变。";
        }
    }
    if (!moved) {
        organizerConfig_ = originalConfig;
        if (showError) {
            MessageDialog::Show(instance_, hwnd_, errorMessage.c_str(), L"桌面项目收纳失败", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    LoadDesktopItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

DesktopItem* MainWindow::FindItem(const std::wstring& itemId) {
    for (DesktopItem& item : items_) {
        if (item.id == itemId) {
            return &item;
        }
    }
    return nullptr;
}

bool MainWindow::IsItemAssigned(const std::wstring& itemId) const {
    if (std::find(organizerConfig_.uncategorizedItemIds.begin(), organizerConfig_.uncategorizedItemIds.end(), itemId) != organizerConfig_.uncategorizedItemIds.end()) {
        return true;
    }
    for (const Category& category : organizerConfig_.categories) {
        if (std::find(category.itemIds.begin(), category.itemIds.end(), itemId) != category.itemIds.end()) {
            return true;
        }
    }
    return false;
}

std::wstring MainWindow::StorageFolderForCategory(const std::wstring& categoryId) const {
    if (categoryId == kUncategorizedCategoryId) {
        return organizerConfig_.uncategorizedStorageFolder.empty()
            ? std::wstring(kUncategorizedCategoryId)
            : organizerConfig_.uncategorizedStorageFolder;
    }
    const Category* category = FindCategory(categoryId);
    if (category == nullptr) {
        return categoryId;
    }
    return category->storageFolder.empty() ? category->id : category->storageFolder;
}

Category* MainWindow::FindCategory(const std::wstring& categoryId) {
    for (Category& category : organizerConfig_.categories) {
        if (category.id == categoryId) {
            return &category;
        }
    }
    return nullptr;
}

const Category* MainWindow::FindCategory(const std::wstring& categoryId) const {
    for (const Category& category : organizerConfig_.categories) {
        if (category.id == categoryId) {
            return &category;
        }
    }
    return nullptr;
}

std::wstring MainWindow::CurrentCategoryName() const {
    if (organizerConfig_.currentCategoryId == kUncategorizedCategoryId) {
        return organizerConfig_.uncategorizedName;
    }
    const Category* category = FindCategory(organizerConfig_.currentCategoryId);
    return category == nullptr ? L"未分类" : category->name;
}

std::wstring MainWindow::GenerateCategoryId() const {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    return L"cat-" + std::to_wstring(time.wYear) +
           std::to_wstring(time.wMonth) +
           std::to_wstring(time.wDay) +
           std::to_wstring(time.wHour) +
           std::to_wstring(time.wMinute) +
           std::to_wstring(time.wSecond) +
           L"-" + std::to_wstring(GetTickCount64());
}
