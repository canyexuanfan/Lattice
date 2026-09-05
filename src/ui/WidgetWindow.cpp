#include "ui/WidgetWindow.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <iterator>

#include "desktop/DesktopScanner.h"
#include "desktop/DesktopLayout.h"
#include "desktop/DesktopPlacementCoordinator.h"
#include "desktop/CategoryStorageManager.h"
#include "app/resource.h"
#include "model/OrganizerModel.h"
#include "shell/ShellDragDrop.h"
#include "shell/ShellDropTarget.h"
#include "ui/InputDialog.h"
#include "ui/DragGhostWindow.h"
#include "ui/MessageDialog.h"
#include "util/PathUtil.h"
#include "util/StringUtil.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"Lattice.WidgetWindow";
constexpr int kTitleHeight = 32;
constexpr FLOAT kHeaderVisualHeight = 24.0f;
constexpr FLOAT kHeaderVisualTop =
    (static_cast<FLOAT>(kTitleHeight) - kHeaderVisualHeight) / 2.0f;
constexpr FLOAT kHeaderControlOffsetY = kHeaderVisualTop - 7.5f;
constexpr int kDefaultWidgetWidth = 390;
constexpr int kResizeGrip = 12;
constexpr int kWheelScrollPixels = 84;
constexpr UINT kIconReadyMessage = WM_APP + 14;
constexpr UINT_PTR kBackdropRefreshTimerId = 4;
constexpr UINT kBackdropRefreshDelayMilliseconds = 80;
constexpr UINT_PTR kIconDragTimerId = 5;
constexpr UINT kIconDragPollMilliseconds = 16;
constexpr wchar_t kAlignmentGuideClassName[] = L"Lattice.AlignmentGuide";
constexpr wchar_t kCurrentVersion[] = L"0.4.32";
constexpr UINT kShellNewCommandFirst = 0x5000;
constexpr UINT kShellNewCommandLast = 0x5FFF;

thread_local IContextMenu2* gActiveShellMenu2 = nullptr;
thread_local IContextMenu3* gActiveShellMenu3 = nullptr;

HWND FindDesktopListView(HWND shellWindow) {
    if (shellWindow == nullptr) {
        return nullptr;
    }
    for (HWND view = FindWindowExW(
             shellWindow, nullptr, L"SHELLDLL_DefView", nullptr);
         view != nullptr;
         view = FindWindowExW(
             shellWindow, view, L"SHELLDLL_DefView", nullptr)) {
        HWND listView = FindWindowExW(view, nullptr, L"SysListView32", nullptr);
        if (listView != nullptr && IsWindow(listView) != FALSE) {
            return listView;
        }
    }
    return nullptr;
}

HWND FindDesktopWidgetHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (HWND listView = FindDesktopListView(progman)) {
        return GetAncestor(listView, GA_ROOT);
    }
    if (progman != nullptr) {
        for (HWND worker = FindWindowExW(
                 progman, nullptr, L"WorkerW", nullptr);
             worker != nullptr;
             worker = FindWindowExW(
                 progman, worker, L"WorkerW", nullptr)) {
            if (HWND listView = FindDesktopListView(worker)) {
                return GetAncestor(listView, GA_ROOT);
            }
        }
    }
    for (HWND worker = FindWindowExW(
             nullptr, nullptr, L"WorkerW", nullptr);
         worker != nullptr;
         worker = FindWindowExW(
             nullptr, worker, L"WorkerW", nullptr)) {
        if (HWND listView = FindDesktopListView(worker)) {
            return GetAncestor(listView, GA_ROOT);
        }
    }
    return nullptr;
}

struct WidgetRectangleEnumerationState {
    HWND current = nullptr;
    DWORD processId = 0;
    std::vector<RECT>* rectangles = nullptr;
};

void CollectWidgetRectangle(
    HWND candidate,
    WidgetRectangleEnumerationState& state) {
    if (candidate == state.current || !IsWindowVisible(candidate)) {
        return;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(candidate, &processId);
    wchar_t className[64]{};
    GetClassNameW(candidate, className, ARRAYSIZE(className));
    if (processId != state.processId ||
        wcscmp(className, kWindowClassName) != 0) {
        return;
    }
    RECT rect{};
    if (GetWindowRect(candidate, &rect)) {
        state.rectangles->push_back(rect);
    }
}

BOOL CALLBACK CollectWidgetChildRectangle(HWND candidate, LPARAM parameter) {
    auto* state = reinterpret_cast<WidgetRectangleEnumerationState*>(parameter);
    CollectWidgetRectangle(candidate, *state);
    return TRUE;
}

BOOL CALLBACK CollectWidgetTopLevelRectangle(HWND candidate, LPARAM parameter) {
    auto* state = reinterpret_cast<WidgetRectangleEnumerationState*>(parameter);
    CollectWidgetRectangle(candidate, *state);
    EnumChildWindows(candidate, CollectWidgetChildRectangle, parameter);
    return TRUE;
}

std::vector<RECT> CollectOtherWidgetRectangles(HWND current) {
    std::vector<RECT> rectangles;
    WidgetRectangleEnumerationState state{
        current,
        GetCurrentProcessId(),
        &rectangles};
    EnumWindows(
        CollectWidgetTopLevelRectangle,
        reinterpret_cast<LPARAM>(&state));
    return rectangles;
}

struct WidgetHandleEnumerationState {
    HWND current = nullptr;
    DWORD processId = 0;
    std::vector<HWND>* windows = nullptr;
};

void CollectSiblingWidgetHandle(
    HWND candidate,
    WidgetHandleEnumerationState& state) {
    if (candidate == state.current || IsWindowVisible(candidate) == FALSE) {
        return;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(candidate, &processId);
    wchar_t className[64]{};
    GetClassNameW(candidate, className, ARRAYSIZE(className));
    if (processId == state.processId &&
        wcscmp(className, kWindowClassName) == 0) {
        state.windows->push_back(candidate);
    }
}

BOOL CALLBACK CollectSiblingWidgetTopLevel(HWND candidate, LPARAM parameter) {
    auto* state = reinterpret_cast<WidgetHandleEnumerationState*>(parameter);
    CollectSiblingWidgetHandle(candidate, *state);
    return TRUE;
}

std::vector<HWND> CollectOtherWidgetWindows(HWND current) {
    std::vector<HWND> windows;
    WidgetHandleEnumerationState state{
        current,
        GetCurrentProcessId(),
        &windows};
    EnumWindows(
        CollectSiblingWidgetTopLevel,
        reinterpret_cast<LPARAM>(&state));
    return windows;
}

class AlignmentGuideOverlay {
public:
    static void Update(
        HINSTANCE instance,
        bool showVertical,
        int verticalX,
        int verticalTop,
        int verticalBottom,
        bool showHorizontal,
        int horizontalY,
        int horizontalLeft,
        int horizontalRight) {
        EnsureWindows(instance);
        PositionLine(VerticalWindow(), showVertical, verticalX - 1, verticalTop, 2, std::max(1, verticalBottom - verticalTop));
        PositionLine(HorizontalWindow(), showHorizontal, horizontalLeft, horizontalY - 1, std::max(1, horizontalRight - horizontalLeft), 2);
    }

    static void Hide() {
        if (VerticalWindow() != nullptr) {
            ShowWindow(VerticalWindow(), SW_HIDE);
        }
        if (HorizontalWindow() != nullptr) {
            ShowWindow(HorizontalWindow(), SW_HIDE);
        }
    }

private:
    static HWND& VerticalWindow() {
        static HWND window = nullptr;
        return window;
    }

    static HWND& HorizontalWindow() {
        static HWND window = nullptr;
        return window;
    }

    static void EnsureWindows(HINSTANCE instance) {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (!GetClassInfoExW(instance, kAlignmentGuideClassName, &existing)) {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.hInstance = instance;
            windowClass.lpfnWndProc = DefWindowProcW;
            windowClass.lpszClassName = kAlignmentGuideClassName;
            windowClass.hbrBackground = CreateSolidBrush(RGB(63, 211, 241));
            RegisterClassExW(&windowClass);
        }
        if (VerticalWindow() == nullptr) {
            VerticalWindow() = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kAlignmentGuideClassName, L"", WS_POPUP, 0, 0, 1, 1,
                nullptr, nullptr, instance, nullptr);
            if (VerticalWindow() != nullptr) {
                SetLayeredWindowAttributes(VerticalWindow(), 0, 220, LWA_ALPHA);
            }
        }
        if (HorizontalWindow() == nullptr) {
            HorizontalWindow() = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kAlignmentGuideClassName, L"", WS_POPUP, 0, 0, 1, 1,
                nullptr, nullptr, instance, nullptr);
            if (HorizontalWindow() != nullptr) {
                SetLayeredWindowAttributes(HorizontalWindow(), 0, 220, LWA_ALPHA);
            }
        }
    }

    static void PositionLine(HWND window, bool show, int x, int y, int width, int height) {
        if (window == nullptr) {
            return;
        }
        if (!show) {
            ShowWindow(window, SW_HIDE);
            return;
        }
        SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
};

bool IsShellMenuMessage(UINT message) {
    return message == WM_INITMENUPOPUP || message == WM_DRAWITEM || message == WM_MEASUREITEM || message == WM_MENUCHAR;
}

bool IsNewMenuText(std::wstring text) {
    text.erase(std::remove(text.begin(), text.end(), L'&'), text.end());
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return text.find(L"新建") != std::wstring::npos || text == L"new" || text.rfind(L"new ", 0) == 0;
}

int FindNewSubmenuPosition(HMENU menu) {
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        wchar_t text[128]{};
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_STRING | MIIM_SUBMENU;
        info.dwTypeData = text;
        info.cch = ARRAYSIZE(text);
        if (GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info) && info.hSubMenu != nullptr && IsNewMenuText(text)) {
            return index;
        }
    }
    return -1;
}

class ScopedPerMonitorV2Awareness {
public:
    explicit ScopedPerMonitorV2Awareness(HWND host = nullptr) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) {
            return;
        }
        setContext_ = reinterpret_cast<SetContextFunction>(
            GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (setContext_ != nullptr) {
            DPI_AWARENESS_CONTEXT target = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
            if (host != nullptr) {
                const auto getContext = reinterpret_cast<GetContextFunction>(
                    GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
                if (getContext != nullptr) {
                    const DPI_AWARENESS_CONTEXT hostContext = getContext(host);
                    if (hostContext != nullptr) {
                        target = hostContext;
                    }
                }
            }
            previous_ = setContext_(target);
        }
    }

    ~ScopedPerMonitorV2Awareness() {
        if (setContext_ != nullptr && previous_ != nullptr) {
            setContext_(previous_);
        }
    }

private:
    using GetContextFunction = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
    using SetContextFunction = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetContextFunction setContext_ = nullptr;
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

void ConfigureDeskGoWindowChrome(HWND hwnd) {
    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
    const BOOL useRedirectionAlpha = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_REDIRECTIONBITMAP_ALPHA, &useRedirectionAlpha, sizeof(useRedirectionAlpha));
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

std::wstring ItemExtension(const DesktopItem& item) {
    return Lowercase(std::filesystem::path(item.path).extension().wstring());
}

ULONGLONG ItemModifiedTime(const DesktopItem& item) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    ULARGE_INTEGER value{};
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    return value.QuadPart;
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

WidgetWindow::WidgetWindow(HINSTANCE instance, HWND owner, std::wstring categoryId, int spawnOffset)
    : instance_(instance),
      owner_(owner),
      categoryId_(std::move(categoryId)),
      spawnOffset_(spawnOffset) {
    LoadConfig();
}

WidgetWindow::~WidgetWindow() {
    Close();
}

bool WidgetWindow::Create() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_SHARED));
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kWindowClassName;
    RegisterClassExW(&windowClass);

    desktopHost_ = FindDesktopWidgetHost();
    const auto createWindow = [&](HWND host) {
        ScopedPerMonitorV2Awareness dpiAwareness(host);
        return CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kWindowClassName,
            categoryName_.c_str(),
            WS_POPUP,
            windowConfig_.x,
            windowConfig_.y,
            windowConfig_.width,
            windowConfig_.height,
            host,
            nullptr,
            instance_,
            this);
    };
    hwnd_ = createWindow(desktopHost_);
    if (hwnd_ == nullptr && desktopHost_ != nullptr) {
        desktopHost_ = nullptr;
        hwnd_ = createWindow(nullptr);
    }
    if (hwnd_ != nullptr) {
        windowConfig_.dpi = GetDpiForWindow(hwnd_);
        if (windowConfig_.collapsed) {
            windowConfig_.height = DipToPixels(kTitleHeight);
            SetWindowBoundsFromScreen(
                windowConfig_.x,
                windowConfig_.y,
                windowConfig_.width,
                windowConfig_.height,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        MaintainDesktopLayer();
        ConfigureDeskGoWindowChrome(hwnd_);
        EnsureWindowVisible();
        RefreshWallpaperBackdrop();
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
    }
    return hwnd_ != nullptr;
}

void WidgetWindow::Show(int showCommand) {
    if (hwnd_ == nullptr) {
        return;
    }
    ShowWindow(hwnd_, showCommand);
    if (showCommand != SW_HIDE) {
        MaintainDesktopLayer();
    }
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
}

void WidgetWindow::SetVisible(bool visible) {
    if (hwnd_ == nullptr) {
        return;
    }
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) {
        MaintainDesktopLayer();
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
    }
}

bool WidgetWindow::IsDesktopHosted() const noexcept {
    return hwnd_ != nullptr &&
           desktopHost_ != nullptr &&
           IsWindow(desktopHost_) != FALSE &&
           (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CHILD) == 0 &&
           GetWindow(hwnd_, GW_OWNER) == desktopHost_;
}

void WidgetWindow::MaintainDesktopLayer() {
    if (hwnd_ == nullptr || IsWindow(hwnd_) == FALSE) {
        return;
    }
    const bool hosted = IsDesktopHosted();
    SetWindowPos(
        hwnd_,
        HWND_BOTTOM,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
            (hosted ? SWP_NOOWNERZORDER : 0));
}

void WidgetWindow::PlaceAboveSiblingWidgets() {
    if (hwnd_ == nullptr || IsWindow(hwnd_) == FALSE) {
        return;
    }
    const bool hosted = IsDesktopHosted();
    const std::vector<HWND> siblings = CollectOtherWidgetWindows(hwnd_);
    for (HWND sibling : siblings) {
        if (hosted && GetWindow(sibling, GW_OWNER) != desktopHost_) {
            continue;
        }
        SetWindowPos(
            sibling,
            hwnd_,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                SWP_NOOWNERZORDER);
    }
}

bool WidgetWindow::SetWindowBoundsFromScreen(
    int x,
    int y,
    int width,
    int height,
    UINT flags) {
    if (hwnd_ == nullptr || IsWindow(hwnd_) == FALSE) {
        return false;
    }
    const bool hosted = IsDesktopHosted();
    UINT effectiveFlags = flags | SWP_NOACTIVATE |
        (hosted ? SWP_NOOWNERZORDER : 0);
    HWND insertAfter = HWND_BOTTOM;
    if (!hosted) {
        effectiveFlags &= ~SWP_NOZORDER;
    }
    return SetWindowPos(
               hwnd_,
               insertAfter,
               x,
               y,
               width,
               height,
               effectiveFlags) != FALSE;
}

void WidgetWindow::SetLocked(bool locked) {
    if (windowConfig_.locked == locked) {
        return;
    }
    windowConfig_.locked = locked;
    SaveLayout();
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::Close() {
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}

void WidgetWindow::RefreshFromConfig() {
    if (hwnd_ != nullptr) {
        PostMessageW(hwnd_, kWidgetRefreshMessage, 0, 0);
    }
}

void WidgetWindow::RefreshIconCache() {
    iconCache_.Clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK WidgetWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    WidgetWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<WidgetWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<WidgetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return window == nullptr ? DefWindowProcW(hwnd, message, wParam, lParam) : window->HandleMessage(message, wParam, lParam);
}

LRESULT WidgetWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (IsShellMenuMessage(message)) {
        if (message == WM_MENUCHAR && gActiveShellMenu3 != nullptr) {
            LRESULT result = 0;
            if (SUCCEEDED(gActiveShellMenu3->HandleMenuMsg2(message, wParam, lParam, &result))) {
                return result;
            }
        } else if (gActiveShellMenu2 != nullptr && SUCCEEDED(gActiveShellMenu2->HandleMenuMsg(message, wParam, lParam))) {
            return 0;
        }
    }
    switch (message) {
        case WM_CREATE:
            shellDropTargetRegistered_ = RegisterShellDropTarget(
                hwnd_,
                [this](const std::vector<std::wstring>& paths, POINT screenPoint) {
                    return QueueDroppedPaths(paths, screenPoint);
                },
                [this]() {
                    return !windowConfig_.collapsed &&
                           !shellDropQueued_ &&
                           !shellDropCommitActive_ &&
                           draggingIconIndex_ < 0 &&
                           !dragVisualActive_;
                },
                [this](
                    ShellDropPreviewEvent event,
                    const std::vector<std::wstring>& paths,
                    POINT screenPoint) {
                    if (event == ShellDropPreviewEvent::Leave) {
                        ClearShellDropPreview(true);
                        return;
                    }
                    UpdateShellDropPreview(
                        paths,
                        screenPoint,
                        event == ShellDropPreviewEvent::Enter);
                });
            if (!shellDropTargetRegistered_) {
                DragAcceptFiles(hwnd_, TRUE);
            }
            d2d_.Initialize(hwnd_, true);
            {
                RECT client{};
                GetClientRect(hwnd_, &client);
                d2d_.Resize(
                    static_cast<UINT>(std::max(0L, client.right - client.left)),
                    static_cast<UINT>(std::max(0L, client.bottom - client.top)));
            }
            RefreshWallpaperBackdrop();
            iconCache_.SetInvalidateCallback([hwnd = hwnd_]() {
                if (hwnd != nullptr && IsWindow(hwnd) != FALSE) {
                    PostMessageW(hwnd, kIconReadyMessage, 0, 0);
                }
            });
            LoadItems();
            RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
            return 0;

        case WM_SIZE:
            d2d_.Resize(LOWORD(lParam), HIWORD(lParam));
            iconGrid_.SetBounds(GridBounds());
            ScheduleWallpaperBackdropRefresh();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_MOVE:
            ScheduleWallpaperBackdropRefresh();
            return 0;

        case WM_MOVING:
            if (!windowConfig_.locked && lParam != 0) {
                ApplyMovingSnap(*reinterpret_cast<RECT*>(lParam));
                return TRUE;
            }
            break;

        case WM_SIZING:
            if (!windowConfig_.locked && !windowConfig_.collapsed && lParam != 0) {
                ApplySizingSnap(*reinterpret_cast<RECT*>(lParam), wParam);
                return TRUE;
            }
            break;

        case WM_ENTERSIZEMOVE:
            AlignmentGuideOverlay::Hide();
            return 0;

        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested != nullptr) {
                SetWindowBoundsFromScreen(
                    suggested->left,
                    suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            windowConfig_.dpi = HIWORD(wParam);
            iconCache_.Clear();
            d2d_.RecreateTarget(hwnd_);
            RefreshWallpaperBackdrop();
            iconGrid_.SetBounds(GridBounds());
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_DISPLAYCHANGE:
            EnsureWindowVisible();
            ScheduleWallpaperBackdropRefresh();
            return 0;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            ScheduleWallpaperBackdropRefresh();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
            minMax->ptMinTrackSize.x = DipToPixels(260);
            minMax->ptMinTrackSize.y = DipToPixels(120);
            return 0;
        }

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_NCHITTEST: {
            POINT pixelPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &pixelPoint);
            const LRESULT resizeHit = ResizeHitTest(pixelPoint);
            if (resizeHit != HTCLIENT) {
                return resizeHit;
            }
            const POINT point = ClientPixelsToDips(pixelPoint);
            if (point.y >= 0 && point.y < kTitleHeight &&
                (HitTestCollapseButton(point) || HitTestLockButton(point) || HeaderButtonAt(point) >= 0)) {
                return HTCLIENT;
            }
            if (!windowConfig_.locked && point.y >= 0 && point.y < kTitleHeight) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_LBUTTONDBLCLK: {
            if (IsShellDropBusy()) {
                return 0;
            }
            const POINT point = ClientPixelsToDips(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            if (point.y < kTitleHeight && !HitTestCollapseButton(point) && !HitTestLockButton(point) && HeaderButtonAt(point) < 0) {
                ToggleCollapsed();
                return 0;
            }
            const int iconIndex = iconGrid_.HitTest(point);
            if (iconIndex >= 0) {
                const DesktopItem* item = iconGrid_.ItemAt(static_cast<size_t>(iconIndex));
                if (item != nullptr && !singleClickOpen_) {
                    launcher_.OpenPath(item->path);
                }
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (IsShellDropBusy()) {
                return 0;
            }
            const POINT pixelPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const POINT point = ClientPixelsToDips(pixelPoint);
            const int headerButton = HeaderButtonAt(point);
            if (headerButton >= 0) {
                pressedHeaderButton_ = headerButton;
                hoverHeaderButton_ = headerButton;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            const int clickedIconIndex = iconGrid_.HitTest(point);
            if (clickedIconIndex >= 0) {
                iconGrid_.SetSelectedIndex(clickedIconIndex);
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (singleClickOpen_) {
                    const DesktopItem* item = iconGrid_.ItemAt(static_cast<size_t>(clickedIconIndex));
                    if (item != nullptr) {
                        launcher_.OpenPath(item->path);
                    }
                    return 0;
                }
            }
            if (!windowConfig_.collapsed &&
                !DragGhostWindow::Instance().IsCommitted()) {
                const int iconIndex = iconGrid_.HitTest(point);
                const DesktopItem* dragItem = iconIndex >= 0
                    ? iconGrid_.ItemAt(static_cast<size_t>(iconIndex))
                    : nullptr;
                if (dragItem != nullptr) {
                    draggingIconIndex_ = iconIndex;
                    draggingItemId_ = dragItem->id;
                    dragTargetIndex_ = iconIndex;
                    dragStartPoint_ = point;
                    SetCapture(hwnd_);
                    SetTimer(hwnd_, kIconDragTimerId, kIconDragPollMilliseconds, nullptr);
                    return 0;
                }
            }
            break;
        }

        case WM_LBUTTONUP:
            if (pressedHeaderButton_ >= 0) {
                const int pressedButton = pressedHeaderButton_;
                const POINT pixelPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const POINT point = ClientPixelsToDips(pixelPoint);
                pressedHeaderButton_ = -1;
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                if (HeaderButtonAt(point) == pressedButton) {
                    InvokeHeaderButton(pressedButton, pixelPoint);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (draggingIconIndex_ >= 0) {
                const POINT pixelPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                FinishIconDrag(pixelPoint);
                return 0;
            }
            break;

        case WM_RBUTTONUP: {
            if (IsShellDropBusy()) {
                return 0;
            }
            const POINT pixelPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT screenPoint = pixelPoint;
            ClientToScreen(hwnd_, &screenPoint);
            const POINT point = ClientPixelsToDips(pixelPoint);
            const int iconIndex = iconGrid_.HitTest(point);
            if (iconIndex >= 0) {
                ShowIconMenu(screenPoint, iconIndex);
                return 0;
            }
            ShowBackgroundMenu(screenPoint);
            return 0;
        }

        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> paths;
            paths.reserve(count);
            for (UINT index = 0; index < count; ++index) {
                const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                if (length == 0) {
                    continue;
                }
                std::vector<wchar_t> path(static_cast<size_t>(length) + 1, L'\0');
                if (DragQueryFileW(drop, index, path.data(), length + 1) > 0) {
                    paths.emplace_back(path.data());
                }
            }
            POINT clientPoint{};
            POINT screenPoint{};
            if (DragQueryPoint(drop, &clientPoint)) {
                screenPoint = clientPoint;
                ClientToScreen(hwnd_, &screenPoint);
            } else {
                GetCursorPos(&screenPoint);
            }
            DragFinish(drop);
            AddDroppedPaths(paths, &screenPoint);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (IsShellDropBusy()) {
                return 0;
            }
            const POINT pixelPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const POINT point = ClientPixelsToDips(pixelPoint);
            UpdateHover(point);
            if (draggingIconIndex_ >= 0 && UpdateIconDrag(pixelPoint)) {
                return 0;
            }
            break;
        }

        case WM_NCMOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            UpdateHover(ClientPixelsToDips(point), true);
            break;
        }

        case WM_MOUSELEAVE:
        case WM_NCMOUSELEAVE:
            hoverIconIndex_ = -1;
            hoverHeaderButton_ = -1;
            headerHovered_ = false;
            iconGrid_.SetHoverIndex(-1);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL:
            if (!windowConfig_.collapsed) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (iconGrid_.ScrollBy(-(delta / WHEEL_DELTA) * kWheelScrollPixels)) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            // A layered drag image or another top-level window can steal capture.
            // Keep the icon drag alive while the physical button is still down;
            // the short-lived timer continues tracking the real cursor globally.
            if (draggingIconIndex_ >= 0 && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                return 0;
            }
            if (draggingIconIndex_ >= 0) {
                POINT pixelPoint{};
                GetCursorPos(&pixelPoint);
                ScreenToClient(hwnd_, &pixelPoint);
                FinishIconDrag(pixelPoint);
                return 0;
            }
            pressedHeaderButton_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_EXITSIZEMOVE:
            AlignmentGuideOverlay::Hide();
            SaveLayout();
            RefreshWallpaperBackdrop();
            return 0;

        case WM_TIMER:
            if (wParam == kIconDragTimerId) {
                if (draggingIconIndex_ < 0) {
                    KillTimer(hwnd_, kIconDragTimerId);
                    return 0;
                }
                POINT pixelPoint{};
                GetCursorPos(&pixelPoint);
                ScreenToClient(hwnd_, &pixelPoint);
                if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                    UpdateIconDrag(pixelPoint);
                } else {
                    FinishIconDrag(pixelPoint);
                }
                return 0;
            }
            if (wParam == kBackdropRefreshTimerId) {
                KillTimer(hwnd_, kBackdropRefreshTimerId);
                RefreshWallpaperBackdrop();
                return 0;
            }
            break;

        case kWidgetShellDropCommitMessage: {
            if (!shellDropQueued_) {
                return 0;
            }
            std::vector<std::wstring> paths =
                std::move(pendingShellDropPaths_);
            std::vector<DesktopDropPosition> desktopPositions =
                std::move(pendingShellDropDesktopPositions_);
            const int insertionIndex = pendingShellDropInsertionIndex_;
            const bool showError = pendingShellDropShowError_;
            pendingShellDropPaths_.clear();
            pendingShellDropDesktopPositions_.clear();
            pendingShellDropInsertionIndex_ = -1;
            pendingShellDropShowError_ = true;
            shellDropQueued_ = false;
            AddDroppedPaths(
                paths,
                nullptr,
                showError,
                &insertionIndex,
                &desktopPositions);
            return 0;
        }

        case kWidgetRefreshMessage:
            if (draggingIconIndex_ >= 0 ||
                dragVisualActive_ ||
                !draggingItemId_.empty() ||
                shellDropPreviewActive_ ||
                shellDropQueued_ ||
                shellDropCommitActive_) {
                refreshPending_ = true;
                return 0;
            }
            refreshPending_ = false;
            LoadConfig();
            LoadItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case kIconReadyMessage:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            Render();
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            iconCache_.SetInvalidateCallback(nullptr);
            pendingShellDropPaths_.clear();
            pendingShellDropInsertionIndex_ = -1;
            pendingShellDropShowError_ = true;
            shellDropQueued_ = false;
            if (draggingIconIndex_ >= 0 || dragVisualActive_) {
                DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration_);
            }
            dragGhostGeneration_ = 0;
            AlignmentGuideOverlay::Hide();
            KillTimer(hwnd_, kIconDragTimerId);
            KillTimer(hwnd_, kBackdropRefreshTimerId);
            if (shellDropTargetRegistered_) {
                UnregisterShellDropTarget(hwnd_);
                shellDropTargetRegistered_ = false;
            } else {
                DragAcceptFiles(hwnd_, FALSE);
            }
            hwnd_ = nullptr;
            if (owner_ != nullptr) {
                PostMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
            }
            return 0;
    }

    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

bool WidgetWindow::LoadConfig() {
    const AppConfig appConfig = configStore_.LoadAppConfig();
    theme_ = appConfig.settings.theme;
    singleClickOpen_ = appConfig.settings.singleClickOpen;
    if (categoryId_ == kUncategorizedCategoryId) {
        categoryName_ = appConfig.uncategorizedName.empty() ? L"未分类" : appConfig.uncategorizedName;
        windowConfig_ = appConfig.window;
        if (windowConfig_.monitorId.empty() && windowConfig_.width == kDefaultWidgetWidth &&
            windowConfig_.x == appConfig.window.x && windowConfig_.y == appConfig.window.y) {
            const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
                windowConfig_.x = info.rcWork.right - windowConfig_.width - 24;
                windowConfig_.y = info.rcWork.top + 96;
            }
        }
        return true;
    }
    for (const CategoryConfig& category : appConfig.categories) {
        if (category.id != categoryId_) {
            continue;
        }
        categoryName_ = category.name;
        windowConfig_ = category.layout;
        if (windowConfig_.monitorId.empty() && windowConfig_.x == 80 && windowConfig_.y == 80 && spawnOffset_ > 0) {
            windowConfig_.x += spawnOffset_;
            windowConfig_.y += spawnOffset_;
        }
        if (windowConfig_.monitorId.empty() && windowConfig_.x == 80 && windowConfig_.y == 80) {
            const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
                windowConfig_.x = info.rcWork.right - windowConfig_.width - 24 - spawnOffset_;
                windowConfig_.y = info.rcWork.top + 96 + spawnOffset_;
            }
        }
        return true;
    }
    return false;
}

void WidgetWindow::EnsureWindowVisible() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    if (MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != nullptr) {
        return;
    }
    const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = std::clamp(rect.left, info.rcWork.left, std::max(info.rcWork.left, info.rcWork.right - width));
    const int y = std::clamp(rect.top, info.rcWork.top, std::max(info.rcWork.top, info.rcWork.bottom - height));
    SetWindowBoundsFromScreen(
        x,
        y,
        0,
        0,
        SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

void WidgetWindow::LoadItems() {
    ++loadItemsGeneration_;
    const AppConfig appConfig = configStore_.LoadAppConfig();
    DesktopScanner scanner;
    items_ = scanner.Scan(appConfig.settings.showPublicDesktopItems);
    for (const ItemConfig& registered : appConfig.items) {
        scanner.MergeRegisteredItem(
            items_,
            registered.id,
            registered.path,
            registered.displayName);
    }
    iconGrid_.SetIconSize(windowConfig_.iconSize);
    iconGrid_.SetDensity(windowConfig_.density);
    iconGrid_.SetCompactStyle(true);
    iconGrid_.SetWidgetStyle(true);
    iconGrid_.SetListMode(windowConfig_.contentViewMode == 1);
    iconGrid_.SetLightTheme(UseLightTheme(appConfig.settings.theme));
    iconCache_.SetCapacity(static_cast<size_t>(appConfig.settings.iconCacheSize));
    RefreshCurrentItems();
    iconGrid_.SetBounds(GridBounds());
}

void WidgetWindow::RefreshCurrentItems() {
    std::vector<DesktopItem> nextItems;
    const AppConfig appConfig = configStore_.LoadAppConfig();
    const auto applyItems = [&]() {
        const bool changed = !SameDesktopItems(currentItems_, nextItems);
        currentItems_ = std::move(nextItems);
        if (changed) {
            iconGrid_.SetItems(currentItems_);
        }
    };
    if (categoryId_ == kUncategorizedCategoryId) {
        for (const std::wstring& itemId : appConfig.uncategorizedItemIds) {
            const auto found = std::find_if(items_.begin(), items_.end(), [&](const DesktopItem& item) {
                return item.id == itemId;
            });
            if (found != items_.end()) {
                nextItems.push_back(*found);
            }
        }
        const int effectiveSortMode = windowConfig_.autoArrange && windowConfig_.sortMode == 0
            ? 1
            : windowConfig_.sortMode;
        if (effectiveSortMode != 0) {
            std::stable_sort(nextItems.begin(), nextItems.end(), [&](const DesktopItem& left, const DesktopItem& right) {
                if (effectiveSortMode == 2) {
                    const std::wstring leftExtension = ItemExtension(left);
                    const std::wstring rightExtension = ItemExtension(right);
                    if (leftExtension != rightExtension) {
                        return leftExtension < rightExtension;
                    }
                } else if (effectiveSortMode == 3) {
                    const ULONGLONG leftTime = ItemModifiedTime(left);
                    const ULONGLONG rightTime = ItemModifiedTime(right);
                    if (leftTime != rightTime) {
                        return leftTime > rightTime;
                    }
                }
                return Lowercase(left.displayName) < Lowercase(right.displayName);
            });
        }
        applyItems();
        return;
    }
    for (const CategoryConfig& category : appConfig.categories) {
        if (category.id != categoryId_) {
            continue;
        }
        for (const std::wstring& itemId : category.itemIds) {
            const auto found = std::find_if(items_.begin(), items_.end(), [&](const DesktopItem& item) {
                return item.id == itemId;
            });
            if (found != items_.end()) {
                nextItems.push_back(*found);
            }
        }
        break;
    }
    const int effectiveSortMode = windowConfig_.autoArrange && windowConfig_.sortMode == 0
        ? 1
        : windowConfig_.sortMode;
    if (effectiveSortMode != 0) {
        std::stable_sort(nextItems.begin(), nextItems.end(), [&](const DesktopItem& left, const DesktopItem& right) {
            if (effectiveSortMode == 2) {
                const std::wstring leftExtension = ItemExtension(left);
                const std::wstring rightExtension = ItemExtension(right);
                if (leftExtension != rightExtension) {
                    return leftExtension < rightExtension;
                }
            } else if (effectiveSortMode == 3) {
                const ULONGLONG leftTime = ItemModifiedTime(left);
                const ULONGLONG rightTime = ItemModifiedTime(right);
                if (leftTime != rightTime) {
                    return leftTime > rightTime;
                }
            }
            return Lowercase(left.displayName) < Lowercase(right.displayName);
        });
    }
    applyItems();
}

bool WidgetWindow::QueueDroppedPaths(
    const std::vector<std::wstring>& paths,
    POINT screenPoint,
    bool showError) {
    if (paths.empty() ||
        IsWindow(hwnd_) == FALSE ||
        shellDropQueued_ ||
        shellDropCommitActive_) {
        return false;
    }

    const bool hasPreviewInsertion =
        shellDropPreviewActive_ && shellDropInsertionIndex_ >= 0;
    int insertionIndex = hasPreviewInsertion
        ? shellDropInsertionIndex_
        : static_cast<int>(currentItems_.size());
    POINT clientPoint = screenPoint;
    const bool hasClientPoint = ScreenToClient(hwnd_, &clientPoint) != FALSE;
    const POINT clientDipPoint = hasClientPoint
        ? ClientPixelsToDips(clientPoint)
        : POINT{};
    if (hasClientPoint) {
        insertionIndex = iconGrid_.InsertionIndexForPoint(clientDipPoint);
    }
    insertionIndex = std::clamp(
        insertionIndex,
        0,
        static_cast<int>(currentItems_.size()));

    std::vector<DesktopDropPosition> desktopPositions;
    DesktopLayout desktopLayout;
    for (const std::wstring& path : paths) {
        if (!shortcutStore_.IsDesktopPath(path)) {
            continue;
        }
        const bool duplicate = std::any_of(
            desktopPositions.begin(),
            desktopPositions.end(),
            [&](const DesktopDropPosition& value) {
                return CompareStringOrdinal(
                           value.path.c_str(),
                           -1,
                           path.c_str(),
                           -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (duplicate) {
            continue;
        }
        POINT point{};
        std::wstring captureError;
        if (!desktopLayout.CapturePosition(path, point, captureError)) {
            if (shortcutStore_.RequiresManagedStorage(path)) {
                return false;
            }
            continue;
        }
        desktopPositions.push_back(DesktopDropPosition{path, point});
    }

    // Drop can provide a new IDataObject or a newer cursor coordinate than the
    // last DragOver. Project the final path sequence at the final release point
    // before the queued frame is painted and any file I/O starts.
    ApplyShellDropProjection(paths, insertionIndex);

    pendingShellDropPaths_ = paths;
    pendingShellDropDesktopPositions_ = std::move(desktopPositions);
    pendingShellDropInsertionIndex_ = insertionIndex;
    pendingShellDropShowError_ = showError;
    shellDropQueued_ = true;
    ClearShellDropPreview(false);
    hoverIconIndex_ = hasClientPoint && !windowConfig_.collapsed
        ? iconGrid_.HitTest(clientDipPoint)
        : -1;
    iconGrid_.SetHoverIndex(hoverIconIndex_);
    iconGrid_.SetSelectedIndex(-1);
    RedrawWindow(
        hwnd_,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW);

    if (PostMessageW(hwnd_, kWidgetShellDropCommitMessage, 0, 0) == FALSE) {
        pendingShellDropPaths_.clear();
        pendingShellDropDesktopPositions_.clear();
        pendingShellDropInsertionIndex_ = -1;
        pendingShellDropShowError_ = true;
        shellDropQueued_ = false;
        if (shellDropProjectionActive_) {
            iconGrid_.SetItems(currentItems_);
            shellDropProjectionActive_ = false;
        }
        RedrawWindow(
            hwnd_,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW);
        FlushDeferredRefresh();
        return false;
    }
    return true;
}

void WidgetWindow::UpdateShellDropPreview(
    const std::vector<std::wstring>& paths,
    POINT screenPoint,
    bool preloadIcons) {
    if (shellDropQueued_ || shellDropCommitActive_) {
        return;
    }
    for (const std::wstring& path : paths) {
        if (preloadIcons) {
            iconCache_.Preload(path);
        }
    }
    POINT clientPoint = screenPoint;
    if (!ScreenToClient(hwnd_, &clientPoint)) {
        return;
    }
    const int insertionIndex = std::clamp(
        iconGrid_.InsertionIndexForPoint(ClientPixelsToDips(clientPoint)),
        0,
        static_cast<int>(currentItems_.size()));
    ApplyShellDropProjection(paths, insertionIndex);
}

void WidgetWindow::ApplyShellDropProjection(
    const std::vector<std::wstring>& paths,
    int insertionIndex) {
    std::vector<std::wstring> projectedPaths;
    projectedPaths.reserve(paths.size());
    for (const std::wstring& path : paths) {
        const bool duplicate = std::any_of(
            projectedPaths.begin(),
            projectedPaths.end(),
            [&](const std::wstring& projectedPath) {
                return CompareStringOrdinal(
                           projectedPath.c_str(),
                           -1,
                           path.c_str(),
                           -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (duplicate) {
            continue;
        }
        projectedPaths.push_back(path);
    }
    insertionIndex = std::clamp(
        insertionIndex,
        0,
        static_cast<int>(currentItems_.size()));
    const bool samePreviewPaths =
        shellDropPreviewPaths_.size() == projectedPaths.size() &&
        std::equal(
            shellDropPreviewPaths_.begin(),
            shellDropPreviewPaths_.end(),
            projectedPaths.begin(),
            [](const std::wstring& left, const std::wstring& right) {
                return CompareStringOrdinal(
                           left.c_str(),
                           -1,
                           right.c_str(),
                           -1,
                           TRUE) == CSTR_EQUAL;
            });
    if (shellDropPreviewActive_ &&
        shellDropInsertionIndex_ == insertionIndex &&
        samePreviewPaths) {
        return;
    }

    ID2D1RenderTarget* iconTarget = d2d_.Target();
    if (iconTarget != nullptr) {
        for (const std::wstring& path : projectedPaths) {
            (void)iconCache_.GetIcon(iconTarget, path);
        }
    }

    DesktopScanner scanner;
    std::vector<DesktopItem> projectedItems = currentItems_;
    size_t projectedIndex = static_cast<size_t>(insertionIndex);
    for (const std::wstring& path : projectedPaths) {
        DesktopItem previewItem = scanner.CreateItemFromPath(path, false);
        projectedItems.insert(
            projectedItems.begin() + static_cast<std::ptrdiff_t>(projectedIndex),
            std::move(previewItem));
        ++projectedIndex;
    }
    hoverIconIndex_ = -1;
    iconGrid_.SetHoverIndex(-1);
    iconGrid_.SetSelectedIndex(-1);
    shellDropPreviewActive_ = true;
    shellDropProjectionActive_ = true;
    shellDropPreviewPaths_ = std::move(projectedPaths);
    shellDropInsertionIndex_ = insertionIndex;
    iconGrid_.SetItems(std::move(projectedItems));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::ClearShellDropPreview(bool flushDeferredRefresh) {
    bool changed =
        shellDropPreviewActive_ || shellDropInsertionIndex_ >= 0 ||
        !shellDropPreviewPaths_.empty();
    shellDropPreviewActive_ = false;
    shellDropPreviewPaths_.clear();
    shellDropInsertionIndex_ = -1;
    if (shellDropProjectionActive_ &&
        !shellDropCommitActive_ &&
        !shellDropQueued_) {
        iconGrid_.SetItems(currentItems_);
        shellDropProjectionActive_ = false;
        changed = true;
    }
    if (changed && hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (flushDeferredRefresh) {
        FlushDeferredRefresh();
    }
}

bool WidgetWindow::AddDroppedPaths(
    const std::vector<std::wstring>& paths,
    const POINT* dropScreenPoint,
    bool showError,
    const int* insertionIndexOverride,
    const std::vector<DesktopDropPosition>* desktopPositions) {
    const bool hasInsertionOverride = insertionIndexOverride != nullptr;
    const bool hasPreviewInsertion =
        shellDropPreviewActive_ && shellDropInsertionIndex_ >= 0;
    const bool projectedFrameAlreadyPainted =
        hasInsertionOverride &&
        shellDropProjectionActive_ &&
        !shellDropPreviewActive_;
    int requestedInsertionIndex = hasInsertionOverride
        ? *insertionIndexOverride
        : hasPreviewInsertion
            ? shellDropInsertionIndex_
            : static_cast<int>(currentItems_.size());
    if (!hasInsertionOverride &&
        !hasPreviewInsertion &&
        dropScreenPoint != nullptr) {
        POINT clientPoint = *dropScreenPoint;
        if (ScreenToClient(hwnd_, &clientPoint)) {
            requestedInsertionIndex =
                iconGrid_.InsertionIndexForPoint(ClientPixelsToDips(clientPoint));
        }
    }
    requestedInsertionIndex = std::clamp(
        requestedInsertionIndex,
        0,
        static_cast<int>(currentItems_.size()));
    shellDropCommitActive_ = true;
    ClearShellDropPreview();
    if (!projectedFrameAlreadyPainted) {
        hoverIconIndex_ = -1;
        iconGrid_.SetHoverIndex(-1);
    }
    const auto redrawCommittedFrame = [&]() {
        if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) {
            RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    };
    const auto finishShellDropCommit = [&](bool committed) {
        if (!committed && shellDropProjectionActive_) {
            iconGrid_.SetItems(currentItems_);
        }
        shellDropProjectionActive_ = false;
        shellDropCommitActive_ = false;
        redrawCommittedFrame();
        FlushDeferredRefresh();
    };
    // QueueDroppedPaths synchronously painted the queued OLE projection before
    // posting this commit. Direct callers still need a stable frame before I/O.
    if (!projectedFrameAlreadyPainted) {
        redrawCommittedFrame();
    }
    if (paths.empty()) {
        finishShellDropCommit(false);
        return false;
    }

    DesktopScanner scanner;
    AppConfig appConfig = configStore_.LoadAppConfig();
    const auto targetItemIds = [&](AppConfig& config) -> std::vector<std::wstring>* {
        if (categoryId_ == kUncategorizedCategoryId) {
            return &config.uncategorizedItemIds;
        }
        const auto category = std::find_if(
            config.categories.begin(),
            config.categories.end(),
            [&](const CategoryConfig& value) {
                return value.id == categoryId_;
            });
        return category == config.categories.end() ? nullptr : &category->itemIds;
    };
    const auto targetLayout = [&](AppConfig& config) -> WindowConfig* {
        if (categoryId_ == kUncategorizedCategoryId) {
            return &config.window;
        }
        const auto category = std::find_if(
            config.categories.begin(),
            config.categories.end(),
            [&](const CategoryConfig& value) {
                return value.id == categoryId_;
            });
        return category == config.categories.end() ? nullptr : &category->layout;
    };

    std::vector<std::wstring>* itemIds = targetItemIds(appConfig);
    WindowConfig* storedLayout = targetLayout(appConfig);
    if (itemIds == nullptr || storedLayout == nullptr) {
        finishShellDropCommit(false);
        if (showError) {
            MessageDialog::Show(
                instance_,
                hwnd_,
                L"目标格子已经不存在，未移动任何内容。",
                L"桌面项目收纳失败",
                MB_OK | MB_ICONERROR);
        }
        return false;
    }

    std::vector<std::wstring> visualOrder;
    visualOrder.reserve(itemIds->size() + paths.size());
    for (const DesktopItem& current : currentItems_) {
        if (std::find(itemIds->begin(), itemIds->end(), current.id) !=
                itemIds->end() &&
            std::find(visualOrder.begin(), visualOrder.end(), current.id) ==
                visualOrder.end()) {
            visualOrder.push_back(current.id);
        }
    }
    for (const std::wstring& itemId : *itemIds) {
        if (std::find(visualOrder.begin(), visualOrder.end(), itemId) ==
            visualOrder.end()) {
            visualOrder.push_back(itemId);
        }
    }
    *itemIds = std::move(visualOrder);
    storedLayout->autoArrange = false;
    storedLayout->sortMode = 0;
    const size_t baseInsertionIndex = static_cast<size_t>(std::clamp(
        requestedInsertionIndex,
        0,
        static_cast<int>(itemIds->size())));

    std::vector<DesktopItem> movedItems;
    movedItems.reserve(paths.size());
    std::vector<std::wstring> processedPaths;
    processedPaths.reserve(paths.size());
    std::wstring firstError;

    for (const std::wstring& path : paths) {
        const bool duplicate = std::any_of(
            processedPaths.begin(),
            processedPaths.end(),
            [&](const std::wstring& processed) {
                return CompareStringOrdinal(
                           processed.c_str(),
                           -1,
                           path.c_str(),
                           -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (duplicate) {
            continue;
        }
        processedPaths.push_back(path);

        DesktopItem probedItem = scanner.CreateItemFromPath(path, false);
        const std::wstring sourceDerivedId = probedItem.id;
        const auto visibleSource = std::find_if(
            items_.begin(),
            items_.end(),
            [&](const DesktopItem& value) {
                return CompareStringOrdinal(
                           value.path.c_str(),
                           -1,
                           path.c_str(),
                           -1,
                           TRUE) == CSTR_EQUAL;
            });
        DesktopItem item = visibleSource != items_.end()
            ? *visibleSource
            : std::move(probedItem);
        const std::wstring sourceVisibleId = item.id;
        if (!shortcutStore_.IsSupportedDesktopItem(item.path)) {
            if (firstError.empty()) {
                firstError =
                    L"该文件、文件夹或快捷方式当前不可访问，未移动该项目。";
            }
            continue;
        }

        const std::wstring sourcePath = item.path;
        const bool physicallyManagedShortcut =
            shortcutStore_.RequiresManagedStorage(sourcePath);
        std::wstring originalDesktopPath;
        POINT originalDesktopPoint{};
        bool hasOriginalDesktopPoint = false;
        if (shortcutStore_.IsDesktopPath(item.path)) {
            originalDesktopPath = item.path;
            const auto capturedPosition = desktopPositions == nullptr
                ? std::vector<DesktopDropPosition>::const_iterator{}
                : std::find_if(
                      desktopPositions->begin(),
                      desktopPositions->end(),
                      [&](const DesktopDropPosition& value) {
                          return CompareStringOrdinal(
                                     value.path.c_str(),
                                     -1,
                                     item.path.c_str(),
                                     -1,
                                     TRUE) == CSTR_EQUAL;
                      });
            if (desktopPositions != nullptr &&
                capturedPosition != desktopPositions->end()) {
                originalDesktopPoint = capturedPosition->point;
            } else {
                DesktopLayout desktopLayout;
                std::wstring captureError;
                if (!desktopLayout.CapturePosition(
                        item.path,
                        originalDesktopPoint,
                        captureError)) {
                    if (physicallyManagedShortcut) {
                        if (firstError.empty()) {
                            firstError = captureError;
                        }
                        continue;
                    }
                } else {
                    hasOriginalDesktopPoint = true;
                }
            }
            if (desktopPositions != nullptr &&
                capturedPosition != desktopPositions->end()) {
                hasOriginalDesktopPoint = true;
            }
        }
        const auto existingByPath = std::find_if(
            appConfig.items.begin(),
            appConfig.items.end(),
            [&](const ItemConfig& existing) {
                return CompareStringOrdinal(
                           existing.path.c_str(),
                           -1,
                           item.path.c_str(),
                           -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (existingByPath != appConfig.items.end()) {
            item.id = existingByPath->id;
        } else if (!scanner.TryCreateManagedItemId(
                       [&](const std::wstring& candidate) {
                           return std::any_of(
                               appConfig.items.begin(),
                               appConfig.items.end(),
                               [&](const ItemConfig& value) {
                                   return value.id == candidate;
                               });
                       },
                       item.id)) {
            if (firstError.empty()) {
                firstError =
                    L"无法为该项目创建安全的唯一标识，未移动该项目。";
            }
            continue;
        }

        ItemConfig visibilityState;
        visibilityState.id = item.id;
        visibilityState.path = sourcePath;
        if (existingByPath != appConfig.items.end()) {
            visibilityState = *existingByPath;
        }
        if (physicallyManagedShortcut) {
            std::wstring visibilityError;
            if (!shortcutStore_.PrepareForManagedStorage(
                    visibilityState, visibilityError)) {
                if (firstError.empty()) {
                    firstError = visibilityError;
                }
                continue;
            }
        }
        bool capturedNewVisibility = false;
        if (!physicallyManagedShortcut) {
            std::wstring visibilityError;
            if (visibilityState.desktopVisibilityMode == 0) {
                if (!shortcutStore_.CaptureAndSuppressDesktopVisibility(
                        sourcePath, visibilityState, visibilityError)) {
                    if (firstError.empty()) {
                        firstError = visibilityError;
                    }
                    continue;
                }
                capturedNewVisibility = visibilityState.desktopVisibilityMode != 0;
            } else if (!shortcutStore_.SuppressDesktopVisibility(visibilityState, visibilityError)) {
                if (firstError.empty()) {
                    firstError = visibilityError;
                }
                continue;
            }
        }

        const AppConfig beforeItemConfig = appConfig;
        std::wstring destinationPath;
        std::wstring errorMessage;
        const auto persistCollectedItem = [&](const std::wstring& storedPath) {
                auto registered = std::find_if(
                    appConfig.items.begin(),
                    appConfig.items.end(),
                    [&](const ItemConfig& value) {
                        return value.id == item.id;
                    });
                if (registered == appConfig.items.end()) {
                    appConfig.items.push_back(
                        ItemConfig{item.id, storedPath, item.displayName});
                    registered = std::prev(appConfig.items.end());
                } else {
                    registered->path = storedPath;
                }
                registered->originalDesktopPath = originalDesktopPath;
                registered->desktopX = originalDesktopPoint.x;
                registered->desktopY = originalDesktopPoint.y;
                registered->hasDesktopPosition = hasOriginalDesktopPoint;
                registered->desktopVisibilityMode = physicallyManagedShortcut
                    ? 0
                    : visibilityState.desktopVisibilityMode;
                registered->desktopVisibilityOriginalFlags = physicallyManagedShortcut
                    ? 0
                    : visibilityState.desktopVisibilityOriginalFlags;
                registered->desktopVisibilityNewStartValue = physicallyManagedShortcut
                    ? -1
                    : visibilityState.desktopVisibilityNewStartValue;
                registered->desktopVisibilityClassicValue = physicallyManagedShortcut
                    ? -1
                    : visibilityState.desktopVisibilityClassicValue;
                if (hasOriginalDesktopPoint) {
                    auto placement = std::find_if(
                        appConfig.desktopLayout.begin(),
                        appConfig.desktopLayout.end(),
                        [&](const DesktopPlacementConfig& value) {
                            return CompareStringOrdinal(
                                       value.path.c_str(),
                                       -1,
                                       originalDesktopPath.c_str(),
                                       -1,
                                       TRUE) == CSTR_EQUAL;
                        });
                    if (placement == appConfig.desktopLayout.end()) {
                        appConfig.desktopLayout.push_back(
                            DesktopPlacementConfig{
                                originalDesktopPath,
                                originalDesktopPoint.x,
                                originalDesktopPoint.y});
                    } else {
                        placement->x = originalDesktopPoint.x;
                        placement->y = originalDesktopPoint.y;
                    }
                }

                std::vector<std::wstring>* destinationIds =
                    targetItemIds(appConfig);
                if (destinationIds == nullptr) {
                    return false;
                }
                size_t insertionIndex =
                    baseInsertionIndex + movedItems.size();
                std::vector<std::wstring> removalIds{item.id};
                const auto addStaleId = [&](const std::wstring& candidate) {
                    if (candidate.empty() || candidate == item.id ||
                        std::find(removalIds.begin(), removalIds.end(), candidate) !=
                            removalIds.end()) {
                        return;
                    }
                    const bool ownedByRegisteredItem = std::any_of(
                        appConfig.items.begin(),
                        appConfig.items.end(),
                        [&](const ItemConfig& value) {
                            return value.id == candidate;
                        });
                    if (!ownedByRegisteredItem) {
                        removalIds.push_back(candidate);
                    }
                };
                addStaleId(sourceDerivedId);
                addStaleId(sourceVisibleId);
                for (const std::wstring& removalId : removalIds) {
                    for (auto found = std::find(
                             destinationIds->begin(),
                             destinationIds->end(),
                             removalId);
                         found != destinationIds->end();
                         found = std::find(
                             destinationIds->begin(),
                             destinationIds->end(),
                             removalId)) {
                        if (static_cast<size_t>(std::distance(
                                destinationIds->begin(),
                                found)) < insertionIndex) {
                            --insertionIndex;
                        }
                        destinationIds->erase(found);
                    }
                }
                for (CategoryConfig& category : appConfig.categories) {
                    for (const std::wstring& removalId : removalIds) {
                        category.itemIds.erase(
                            std::remove(
                                category.itemIds.begin(),
                                category.itemIds.end(),
                                removalId),
                            category.itemIds.end());
                    }
                }
                for (const std::wstring& removalId : removalIds) {
                    appConfig.uncategorizedItemIds.erase(
                        std::remove(
                            appConfig.uncategorizedItemIds.begin(),
                            appConfig.uncategorizedItemIds.end(),
                            removalId),
                        appConfig.uncategorizedItemIds.end());
                }
                destinationIds = targetItemIds(appConfig);
                if (destinationIds == nullptr) {
                    return false;
                }
                destinationIds->insert(
                    destinationIds->begin() +
                        static_cast<std::ptrdiff_t>(
                            std::min(insertionIndex, destinationIds->size())),
                    item.id);
                const bool saved = configStore_.SaveAppConfig(appConfig);
                return saved;
            };
        bool collected = false;
        if (physicallyManagedShortcut) {
            collected = shortcutStore_.MoveIntoCategory(
                item.id,
                sourcePath,
                CategoryStorageFolder(appConfig, categoryId_),
                persistCollectedItem,
                destinationPath,
                errorMessage);
        } else {
            destinationPath = sourcePath;
            collected = persistCollectedItem(destinationPath);
            if (!collected) {
                errorMessage = L"无法保存该文件或文件夹的收纳配置，原件未发生改变。";
            }
        }
        if (!collected) {
            appConfig = beforeItemConfig;
            if (capturedNewVisibility) {
                std::wstring rollbackError;
                shortcutStore_.RestoreDesktopVisibility(visibilityState, rollbackError);
            }
            if (firstError.empty()) {
                firstError = errorMessage;
            }
            break;
        }

        if (CompareStringOrdinal(
                sourcePath.c_str(),
                -1,
                destinationPath.c_str(),
                -1,
                TRUE) != CSTR_EQUAL) {
            iconCache_.Alias(sourcePath, destinationPath);
        }
        const DesktopItem destinationMetadata =
            scanner.CreateItemFromPath(destinationPath, false);
        DesktopItem movedItem = item;
        movedItem.path = destinationPath;
        movedItem.displayName = destinationMetadata.displayName;
        movedItem.kind = destinationMetadata.kind;
        movedItem.missing = destinationMetadata.missing;
        movedItem.id = item.id;
        const auto registered = std::find_if(
            appConfig.items.begin(),
            appConfig.items.end(),
            [&](const ItemConfig& value) {
                return value.id == item.id;
            });
        if (registered != appConfig.items.end() &&
            !registered->displayName.empty()) {
            movedItem.displayName = registered->displayName;
        }
        items_.erase(
            std::remove_if(
                items_.begin(),
                items_.end(),
                [&](const DesktopItem& value) {
                    return value.id == item.id ||
                           CompareStringOrdinal(
                               value.path.c_str(),
                               -1,
                               sourcePath.c_str(),
                               -1,
                               TRUE) == CSTR_EQUAL;
                }),
            items_.end());
        items_.push_back(movedItem);
        movedItems.push_back(std::move(movedItem));
    }

    if (!movedItems.empty()) {
        windowConfig_.autoArrange = false;
        windowConfig_.sortMode = 0;
        currentItems_.clear();
        const std::vector<std::wstring>* finalItemIds =
            targetItemIds(appConfig);
        if (finalItemIds != nullptr) {
            for (const std::wstring& itemId : *finalItemIds) {
                const auto found = std::find_if(
                    items_.begin(),
                    items_.end(),
                    [&](const DesktopItem& value) {
                        return value.id == itemId;
                    });
                if (found != items_.end()) {
                    currentItems_.push_back(*found);
                }
            }
        }
        iconGrid_.SetItems(currentItems_);
        if (owner_ != nullptr && IsWindow(owner_) != FALSE) {
            PostMessageW(owner_, kOrganizerConfigSyncMessage, 0, 0);
        }
    }
    finishShellDropCommit(!movedItems.empty());
    if (showError && !firstError.empty()) {
        MessageDialog::Show(
            instance_,
            hwnd_,
            firstError.c_str(),
            L"桌面项目收纳失败",
            MB_OK | MB_ICONERROR);
    }
    return !movedItems.empty();
}

void WidgetWindow::SaveLayout() {
    if (categoryId_.empty()) {
        return;
    }
    RECT rect{};
    if (hwnd_ != nullptr && GetWindowRect(hwnd_, &rect)) {
        windowConfig_.x = rect.left;
        windowConfig_.y = rect.top;
        windowConfig_.width = rect.right - rect.left;
        windowConfig_.height = rect.bottom - rect.top;
    }
    windowConfig_.dpi = hwnd_ == nullptr ? windowConfig_.dpi : GetDpiForWindow(hwnd_);
    HMONITOR monitor = hwnd_ == nullptr ? nullptr : MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        windowConfig_.monitorId = monitorInfo.szDevice;
    }

    AppConfig appConfig = configStore_.LoadAppConfig();
    if (categoryId_ == kUncategorizedCategoryId) {
        appConfig.window = windowConfig_;
    } else {
        for (CategoryConfig& category : appConfig.categories) {
            if (category.id == categoryId_) {
                category.layout = windowConfig_;
                break;
            }
        }
    }
    if (configStore_.SaveAppConfig(appConfig) && owner_ != nullptr && IsWindow(owner_)) {
        SendMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
    }
}

void WidgetWindow::ReorderItem(size_t fromIndex, size_t toIndex) {
    if (fromIndex >= currentItems_.size() || toIndex >= currentItems_.size() || fromIndex == toIndex) {
        return;
    }
    windowConfig_.autoArrange = false;
    windowConfig_.sortMode = 0;
    AppConfig appConfig = configStore_.LoadAppConfig();
    std::vector<std::wstring>* itemIds = &appConfig.uncategorizedItemIds;
    WindowConfig* storedLayout = &appConfig.window;
    for (CategoryConfig& category : appConfig.categories) {
        if (category.id == categoryId_) {
            itemIds = &category.itemIds;
            storedLayout = &category.layout;
            break;
        }
    }
    storedLayout->autoArrange = false;
    storedLayout->sortMode = 0;
    if (itemIds != nullptr) {
        const std::wstring movingId = currentItems_[fromIndex].id;
        const std::wstring targetId = currentItems_[toIndex].id;
        auto fromIt = std::find(itemIds->begin(), itemIds->end(), movingId);
        auto targetIt = std::find(itemIds->begin(), itemIds->end(), targetId);
        if (fromIt == itemIds->end() || targetIt == itemIds->end()) {
            return;
        }
        const size_t targetPosition = static_cast<size_t>(std::distance(itemIds->begin(), targetIt));
        itemIds->erase(fromIt);
        itemIds->insert(
            itemIds->begin() + static_cast<std::ptrdiff_t>(std::min(targetPosition, itemIds->size())),
            movingId);
    }
    configStore_.SaveAppConfig(appConfig);
    RefreshCurrentItems();
    PostMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::ToggleCollapsed() {
    if (windowConfig_.fixedExpanded && !windowConfig_.collapsed) {
        return;
    }
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    windowConfig_.x = rect.left;
    windowConfig_.y = rect.top;
    windowConfig_.width = rect.right - rect.left;
    if (!windowConfig_.collapsed) {
        windowConfig_.normalHeight = rect.bottom - rect.top;
    }
    const bool expanding = windowConfig_.collapsed;
    windowConfig_.collapsed = !windowConfig_.collapsed;
    windowConfig_.height = windowConfig_.collapsed
        ? DipToPixels(kTitleHeight)
        : std::max(windowConfig_.normalHeight, DipToPixels(120));
    SetWindowBoundsFromScreen(
        windowConfig_.x,
        windowConfig_.y,
        windowConfig_.width,
        windowConfig_.height,
        SWP_NOZORDER);
    if (expanding) {
        PlaceAboveSiblingWidgets();
    }
    SaveLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::ToggleLocked() {
    SetLocked(!windowConfig_.locked);
}

void WidgetWindow::SetIconSize(int iconSize) {
    windowConfig_.iconSize = std::clamp(iconSize, 32, 72);
    iconGrid_.SetIconSize(windowConfig_.iconSize);
    SaveLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::SetContentViewMode(int mode) {
    windowConfig_.contentViewMode = std::clamp(mode, 0, 1);
    iconGrid_.SetListMode(windowConfig_.contentViewMode == 1);
    SaveLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::SetSortMode(int mode) {
    windowConfig_.sortMode = std::clamp(mode, 0, 3);
    RefreshCurrentItems();
    SaveLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::SetAutoArrange(bool enabled) {
    windowConfig_.autoArrange = enabled;
    RefreshCurrentItems();
    SaveLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::SetFixedExpanded(bool enabled) {
    windowConfig_.fixedExpanded = enabled;
    if (enabled && windowConfig_.collapsed) {
        windowConfig_.collapsed = false;
        windowConfig_.height = std::max(windowConfig_.normalHeight, DipToPixels(120));
        SetWindowBoundsFromScreen(
            windowConfig_.x,
            windowConfig_.y,
            windowConfig_.width,
            windowConfig_.height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        PlaceAboveSiblingWidgets();
    }
    SaveLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::OpenCategoryLocation() {
    const std::wstring storageFolder = CategoryStorageFolder(configStore_.LoadAppConfig(), categoryId_);
    if (shortcutStore_.EnsureCategoryDirectory(storageFolder)) {
        launcher_.OpenPath(shortcutStore_.CategoryPath(storageFolder));
    }
}

void WidgetWindow::ApplyMovingSnap(RECT& movingRect) const {
    const int width = movingRect.right - movingRect.left;
    const int height = movingRect.bottom - movingRect.top;
    const int alignmentThreshold = DipToPixels(4);
    const int adjacencyThreshold = DipToPixels(2);
    int bestX = movingRect.left;
    int bestY = movingRect.top;
    int bestDx = alignmentThreshold + 1;
    int bestDy = alignmentThreshold + 1;
    int bestGuideX = 0;
    int bestGuideXTop = 0;
    int bestGuideXBottom = 0;
    int bestGuideY = 0;
    int bestGuideYLeft = 0;
    int bestGuideYRight = 0;

    const auto considerX = [&](int candidate, int guideX, int guideTop, int guideBottom, int threshold) {
        const int delta = std::abs(candidate - movingRect.left);
        if (delta < bestDx && delta <= threshold) {
            bestDx = delta;
            bestX = candidate;
            bestGuideX = guideX;
            bestGuideXTop = guideTop;
            bestGuideXBottom = guideBottom;
        }
    };
    const auto considerY = [&](int candidate, int guideY, int guideLeft, int guideRight, int threshold) {
        const int delta = std::abs(candidate - movingRect.top);
        if (delta < bestDy && delta <= threshold) {
            bestDy = delta;
            bestY = candidate;
            bestGuideY = guideY;
            bestGuideYLeft = guideLeft;
            bestGuideYRight = guideRight;
        }
    };

    HMONITOR monitor = MonitorFromRect(&movingRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        considerX(monitorInfo.rcWork.left, monitorInfo.rcWork.left, monitorInfo.rcWork.top, monitorInfo.rcWork.bottom, alignmentThreshold);
        considerX(monitorInfo.rcWork.right - width, monitorInfo.rcWork.right, monitorInfo.rcWork.top, monitorInfo.rcWork.bottom, alignmentThreshold);
        considerY(monitorInfo.rcWork.top, monitorInfo.rcWork.top, monitorInfo.rcWork.left, monitorInfo.rcWork.right, alignmentThreshold);
        considerY(monitorInfo.rcWork.bottom - height, monitorInfo.rcWork.bottom, monitorInfo.rcWork.left, monitorInfo.rcWork.right, alignmentThreshold);
    }

    const std::vector<RECT> otherRectangles =
        CollectOtherWidgetRectangles(hwnd_);
    for (const RECT& other : otherRectangles) {
        const int guideTop = std::min(movingRect.top, other.top);
        const int guideBottom = std::max(movingRect.bottom, other.bottom);
        const int guideLeft = std::min(movingRect.left, other.left);
        const int guideRight = std::max(movingRect.right, other.right);
        considerX(other.left, other.left, guideTop, guideBottom, alignmentThreshold);
        considerX(other.right - width, other.right, guideTop, guideBottom, alignmentThreshold);
        considerX(other.left - width, other.left, guideTop, guideBottom, adjacencyThreshold);
        considerX(other.right, other.right, guideTop, guideBottom, adjacencyThreshold);
        considerX((other.left + other.right - width) / 2, (other.left + other.right) / 2, guideTop, guideBottom, alignmentThreshold);
        considerY(other.top, other.top, guideLeft, guideRight, alignmentThreshold);
        considerY(other.bottom - height, other.bottom, guideLeft, guideRight, alignmentThreshold);
        considerY(other.top - height, other.top, guideLeft, guideRight, adjacencyThreshold);
        considerY(other.bottom, other.bottom, guideLeft, guideRight, adjacencyThreshold);
        considerY((other.top + other.bottom - height) / 2, (other.top + other.bottom) / 2, guideLeft, guideRight, alignmentThreshold);
    }

    movingRect.left = bestX;
    movingRect.top = bestY;
    movingRect.right = bestX + width;
    movingRect.bottom = bestY + height;
    AlignmentGuideOverlay::Update(
        instance_,
        bestDx <= alignmentThreshold,
        bestGuideX,
        bestGuideXTop,
        bestGuideXBottom,
        bestDy <= alignmentThreshold,
        bestGuideY,
        bestGuideYLeft,
        bestGuideYRight);
}

void WidgetWindow::ApplySizingSnap(RECT& sizingRect, WPARAM sizingEdge) const {
    const int threshold = DipToPixels(4);
    const int minimumWidth = DipToPixels(260);
    const int minimumHeight = DipToPixels(120);
    const bool resizeLeft = sizingEdge == WMSZ_LEFT || sizingEdge == WMSZ_TOPLEFT || sizingEdge == WMSZ_BOTTOMLEFT;
    const bool resizeRight = sizingEdge == WMSZ_RIGHT || sizingEdge == WMSZ_TOPRIGHT || sizingEdge == WMSZ_BOTTOMRIGHT;
    const bool resizeTop = sizingEdge == WMSZ_TOP || sizingEdge == WMSZ_TOPLEFT || sizingEdge == WMSZ_TOPRIGHT;
    const bool resizeBottom = sizingEdge == WMSZ_BOTTOM || sizingEdge == WMSZ_BOTTOMLEFT || sizingEdge == WMSZ_BOTTOMRIGHT;
    int bestDx = threshold + 1;
    int bestDy = threshold + 1;
    int bestEdgeX = 0;
    int bestEdgeY = 0;
    int bestGuideXTop = 0;
    int bestGuideXBottom = 0;
    int bestGuideYLeft = 0;
    int bestGuideYRight = 0;

    const auto considerX = [&](int candidate, int guideTop, int guideBottom) {
        const int current = resizeLeft ? sizingRect.left : sizingRect.right;
        const int delta = std::abs(candidate - current);
        const bool keepsMinimum = resizeLeft
            ? sizingRect.right - candidate >= minimumWidth
            : candidate - sizingRect.left >= minimumWidth;
        if (keepsMinimum && delta < bestDx && delta <= threshold) {
            bestDx = delta;
            bestEdgeX = candidate;
            bestGuideXTop = guideTop;
            bestGuideXBottom = guideBottom;
        }
    };
    const auto considerY = [&](int candidate, int guideLeft, int guideRight) {
        const int current = resizeTop ? sizingRect.top : sizingRect.bottom;
        const int delta = std::abs(candidate - current);
        const bool keepsMinimum = resizeTop
            ? sizingRect.bottom - candidate >= minimumHeight
            : candidate - sizingRect.top >= minimumHeight;
        if (keepsMinimum && delta < bestDy && delta <= threshold) {
            bestDy = delta;
            bestEdgeY = candidate;
            bestGuideYLeft = guideLeft;
            bestGuideYRight = guideRight;
        }
    };

    HMONITOR monitor = MonitorFromRect(&sizingRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        if (resizeLeft || resizeRight) {
            considerX(monitorInfo.rcWork.left, monitorInfo.rcWork.top, monitorInfo.rcWork.bottom);
            considerX(monitorInfo.rcWork.right, monitorInfo.rcWork.top, monitorInfo.rcWork.bottom);
        }
        if (resizeTop || resizeBottom) {
            considerY(monitorInfo.rcWork.top, monitorInfo.rcWork.left, monitorInfo.rcWork.right);
            considerY(monitorInfo.rcWork.bottom, monitorInfo.rcWork.left, monitorInfo.rcWork.right);
        }
    }

    const std::vector<RECT> otherRectangles =
        CollectOtherWidgetRectangles(hwnd_);
    for (const RECT& other : otherRectangles) {
        if (resizeLeft || resizeRight) {
            const int guideTop = std::min(sizingRect.top, other.top);
            const int guideBottom = std::max(sizingRect.bottom, other.bottom);
            considerX(other.left, guideTop, guideBottom);
            considerX(other.right, guideTop, guideBottom);
        }
        if (resizeTop || resizeBottom) {
            const int guideLeft = std::min(sizingRect.left, other.left);
            const int guideRight = std::max(sizingRect.right, other.right);
            considerY(other.top, guideLeft, guideRight);
            considerY(other.bottom, guideLeft, guideRight);
        }
    }

    if (bestDx <= threshold) {
        if (resizeLeft) {
            sizingRect.left = bestEdgeX;
        } else if (resizeRight) {
            sizingRect.right = bestEdgeX;
        }
    }
    if (bestDy <= threshold) {
        if (resizeTop) {
            sizingRect.top = bestEdgeY;
        } else if (resizeBottom) {
            sizingRect.bottom = bestEdgeY;
        }
    }
    AlignmentGuideOverlay::Update(
        instance_,
        bestDx <= threshold,
        bestEdgeX,
        bestGuideXTop,
        bestGuideXBottom,
        bestDy <= threshold,
        bestEdgeY,
        bestGuideYLeft,
        bestGuideYRight);
}

void WidgetWindow::OpenDataLocation() {
    const std::filesystem::path configPath(configStore_.ConfigPath());
    if (!configPath.parent_path().empty()) {
        launcher_.OpenPath(configPath.parent_path().wstring());
    }
}

void WidgetWindow::PasteClipboardShortcuts() {
    if (!OpenClipboard(hwnd_)) {
        return;
    }

    std::vector<std::wstring> paths;
    HDROP drop = reinterpret_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (drop != nullptr) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < count; ++index) {
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(drop, index, path, ARRAYSIZE(path)) > 0) {
                paths.emplace_back(path);
            }
        }
    }
    CloseClipboard();

    const bool changed = AddDroppedPaths(paths, nullptr, false);
    if (!changed && !paths.empty()) {
        MessageDialog::Show(instance_,
            hwnd_,
            L"剪贴板中没有可收纳的文件、文件夹或快捷方式。",
            L"Lattice",
            MB_OK | MB_ICONINFORMATION);
    }
}

void WidgetWindow::OpenFeedbackDraft() {
    const std::filesystem::path configPath(configStore_.ConfigPath());
    const std::filesystem::path feedbackPath = configPath.parent_path() / L"Lattice-问题反馈.txt";
    HANDLE file = CreateFileW(feedbackPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
            const std::wstring text =
                std::wstring(L"Lattice 问题反馈\r\n\r\n") +
                L"请描述遇到的问题：\r\n\r\n"
                L"复现步骤：\r\n1. \r\n2. \r\n\r\n"
                L"期望结果：\r\n\r\n"
                L"当前版本：" + kCurrentVersion + L"\r\n";
            const WORD bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(file, &bom, sizeof(bom), &written, nullptr);
            WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
        }
        CloseHandle(file);
        launcher_.OpenPath(feedbackPath.wstring());
    }
}

void WidgetWindow::ShowPersonalCenter() {
    wchar_t userName[256]{};
    DWORD userNameLength = ARRAYSIZE(userName);
    if (!GetUserNameW(userName, &userNameLength)) {
        wcscpy_s(userName, L"Windows 用户");
    }
    const AppConfig config = configStore_.LoadAppConfig();
    const std::wstring message = std::wstring(L"当前用户：") + userName +
        L"\n格子数量：" + std::to_wstring(config.categories.size()) +
        L"\n当前版本：" + kCurrentVersion +
        L"\n\n整理数据保存在当前用户的 Lattice 数据目录中。";
    MessageDialog::Show(instance_, hwnd_, message.c_str(), L"个人中心", MB_OK | MB_ICONINFORMATION);
}

void WidgetWindow::CheckForUpdates() {
    if (owner_ != nullptr && IsWindow(owner_) != FALSE) {
        SendMessageW(owner_, kWidgetHostCommandMessage, static_cast<WPARAM>(WidgetHostCommand::CheckForUpdates), reinterpret_cast<LPARAM>(hwnd_));
    }
}

void WidgetWindow::ShowSortMenu(POINT screenPoint) {
    constexpr int kCustomCommand = 201;
    constexpr int kNameCommand = 202;
    constexpr int kTypeCommand = 203;
    constexpr int kModifiedCommand = 204;
    constexpr int kAutoArrangeCommand = 205;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (windowConfig_.sortMode == 0 ? MF_CHECKED : 0), kCustomCommand, L"自定义顺序");
    AppendMenuW(menu, MF_STRING | (windowConfig_.sortMode == 1 ? MF_CHECKED : 0), kNameCommand, L"按名称排列");
    AppendMenuW(menu, MF_STRING | (windowConfig_.sortMode == 2 ? MF_CHECKED : 0), kTypeCommand, L"按类型排列");
    AppendMenuW(menu, MF_STRING | (windowConfig_.sortMode == 3 ? MF_CHECKED : 0), kModifiedCommand, L"按修改时间排列");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (windowConfig_.autoArrange ? MF_CHECKED : 0), kAutoArrangeCommand, L"自动排列图标");
    const int command = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD,
        screenPoint.x,
        screenPoint.y,
        0,
        hwnd_,
        nullptr);
    DestroyMenu(menu);
    if (command >= kCustomCommand && command <= kModifiedCommand) {
        SetSortMode(command - kCustomCommand);
    } else if (command == kAutoArrangeCommand) {
        SetAutoArrange(!windowConfig_.autoArrange);
    }
}

void WidgetWindow::ShowBackgroundMenu(POINT screenPoint) {
    constexpr int kToggleCollapseCommand = 1;
    constexpr int kFixedExpandedCommand = 2;
    constexpr int kToggleLockCommand = 3;
    constexpr int kOpenLocationCommand = 4;
    constexpr int kRenameCommand = 5;
    constexpr int kRefreshCommand = 6;
    constexpr int kIconSmallCommand = 7;
    constexpr int kIconMediumCommand = 8;
    constexpr int kIconLargeCommand = 9;
    constexpr int kGridModeCommand = 10;
    constexpr int kListModeCommand = 11;
    constexpr int kSortCustomCommand = 12;
    constexpr int kSortNameCommand = 13;
    constexpr int kSortTypeCommand = 14;
    constexpr int kSortModifiedCommand = 15;
    constexpr int kAutoArrangeCommand = 16;
    constexpr int kNewCategoryCommand = 17;
    constexpr int kDissolveCommand = 18;
    constexpr int kImportUnassignedCommand = 19;
    constexpr int kExportCategoryCommand = 20;
    constexpr int kImportCategoryCommand = 21;
    constexpr int kMoveCategoryUpCommand = 22;
    constexpr int kMoveCategoryDownCommand = 23;
    constexpr int kHideAllCommand = 24;
    constexpr int kToggleAllLockedCommand = 25;
    constexpr int kRefreshAllCommand = 26;
    constexpr int kToggleStartupCommand = 27;
    constexpr int kSettingsCommand = 28;
    constexpr int kExportConfigCommand = 29;
    constexpr int kImportConfigCommand = 30;
    constexpr int kOpenDataCommand = 31;
    constexpr int kExitApplicationCommand = 32;
    constexpr int kPasteCommand = 33;
    constexpr int kPasteShortcutCommand = 34;
    constexpr int kFeedbackCommand = 35;
    constexpr int kPersonalCenterCommand = 36;
    constexpr int kCheckUpdateCommand = 37;

    HMENU menu = CreatePopupMenu();
    HMENU shellRootMenu = nullptr;
    Microsoft::WRL::ComPtr<IContextMenu> shellContextMenu;
    Microsoft::WRL::ComPtr<IContextMenu2> shellContextMenu2;
    Microsoft::WRL::ComPtr<IContextMenu3> shellContextMenu3;
    const AppConfig currentConfig = configStore_.LoadAppConfig();
    const UINT pasteState = IsClipboardFormatAvailable(CF_HDROP) != FALSE ? MF_STRING : MF_GRAYED;
    HMENU sizeMenu = CreatePopupMenu();
    AppendMenuW(sizeMenu, MF_STRING | (windowConfig_.iconSize <= 32 ? MF_CHECKED : 0), kIconSmallCommand, L"小图标");
    AppendMenuW(sizeMenu, MF_STRING | (windowConfig_.iconSize > 32 && windowConfig_.iconSize < 64 ? MF_CHECKED : 0), kIconMediumCommand, L"中等图标");
    AppendMenuW(sizeMenu, MF_STRING | (windowConfig_.iconSize >= 64 ? MF_CHECKED : 0), kIconLargeCommand, L"大图标");

    HMENU modeMenu = CreatePopupMenu();
    AppendMenuW(modeMenu, MF_STRING | (windowConfig_.contentViewMode == 0 ? MF_CHECKED : 0), kGridModeCommand, L"图标显示");
    AppendMenuW(modeMenu, MF_STRING | (windowConfig_.contentViewMode == 1 ? MF_CHECKED : 0), kListModeCommand, L"列表显示");
    AppendMenuW(modeMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizeMenu), L"图标大小");
    AppendMenuW(modeMenu, MF_STRING | (windowConfig_.autoArrange ? MF_CHECKED : 0), kAutoArrangeCommand, L"自动排列图标");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modeMenu), L"查看(V)");

    HMENU sortMenu = CreatePopupMenu();
    AppendMenuW(sortMenu, MF_STRING | (windowConfig_.sortMode == 0 ? MF_CHECKED : 0), kSortCustomCommand, L"自定义顺序");
    AppendMenuW(sortMenu, MF_STRING | (windowConfig_.sortMode == 1 ? MF_CHECKED : 0), kSortNameCommand, L"名称");
    AppendMenuW(sortMenu, MF_STRING | (windowConfig_.sortMode == 2 ? MF_CHECKED : 0), kSortTypeCommand, L"类型");
    AppendMenuW(sortMenu, MF_STRING | (windowConfig_.sortMode == 3 ? MF_CHECKED : 0), kSortModifiedCommand, L"修改时间");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式(O)");
    AppendMenuW(menu, MF_STRING, kRefreshCommand, L"刷新(E)");
    AppendMenuW(menu, pasteState, kPasteCommand, L"粘贴(P)");
    AppendMenuW(menu, pasteState, kPasteShortcutCommand, L"粘贴快捷方式(S)");

    bool shellNewMenuAdded = false;
    const std::wstring storageFolder = CategoryStorageFolder(configStore_.LoadAppConfig(), categoryId_);
    if (shortcutStore_.EnsureCategoryDirectory(storageFolder)) {
        Microsoft::WRL::ComPtr<IShellItem> folderItem;
        Microsoft::WRL::ComPtr<IShellFolder> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                shortcutStore_.CategoryPath(storageFolder).c_str(), nullptr, IID_PPV_ARGS(&folderItem))) &&
            SUCCEEDED(folderItem->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder))) &&
            SUCCEEDED(folder->CreateViewObject(hwnd_, IID_PPV_ARGS(&shellContextMenu)))) {
            shellRootMenu = CreatePopupMenu();
            if (SUCCEEDED(shellContextMenu->QueryContextMenu(
                    shellRootMenu,
                    0,
                    kShellNewCommandFirst,
                    kShellNewCommandLast,
                    CMF_NORMAL | CMF_EXPLORE))) {
                const int newPosition = FindNewSubmenuPosition(shellRootMenu);
                if (newPosition >= 0) {
                    MENUITEMINFOW info{};
                    info.cbSize = sizeof(info);
                    info.fMask = MIIM_SUBMENU;
                    if (GetMenuItemInfoW(shellRootMenu, static_cast<UINT>(newPosition), TRUE, &info) && info.hSubMenu != nullptr) {
                        RemoveMenu(shellRootMenu, static_cast<UINT>(newPosition), MF_BYPOSITION);
                        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(info.hSubMenu), L"新建(W)");
                        shellNewMenuAdded = true;
                        shellContextMenu.As(&shellContextMenu2);
                        shellContextMenu.As(&shellContextMenu3);
                    }
                }
            }
        }
    }
    if (!shellNewMenuAdded) {
        AppendMenuW(menu, MF_GRAYED, 0, L"新建(W)");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kRenameCommand, L"重命名格子(M)");
    AppendMenuW(menu, MF_STRING, kNewCategoryCommand, L"新建格子");
    AppendMenuW(
        menu,
        categoryId_ == kUncategorizedCategoryId ? MF_GRAYED : MF_STRING,
        kDissolveCommand,
        L"解散该格子");

    HMENU utilityMenu = CreatePopupMenu();
    AppendMenuW(
        utilityMenu,
        MF_STRING | (windowConfig_.fixedExpanded && !windowConfig_.collapsed ? MF_GRAYED : 0),
        kToggleCollapseCommand,
        windowConfig_.collapsed ? L"展开格子" : L"收起格子");
    AppendMenuW(utilityMenu, MF_STRING | (windowConfig_.fixedExpanded ? MF_CHECKED : 0), kFixedExpandedCommand, L"固定为展开状态");
    AppendMenuW(utilityMenu, MF_STRING, kToggleLockCommand, windowConfig_.locked ? L"解锁格子" : L"锁定格子");
    AppendMenuW(utilityMenu, MF_STRING, kOpenLocationCommand, L"打开格子所在位置");
    {
        HMENU categoryMenu = CreatePopupMenu();
        const UINT categoryOnlyState = categoryId_ == kUncategorizedCategoryId ? MF_GRAYED : MF_STRING;
        AppendMenuW(categoryMenu, categoryOnlyState, kImportUnassignedCommand, L"收纳未分类项目到此格子");
        AppendMenuW(categoryMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(categoryMenu, categoryOnlyState, kExportCategoryCommand, L"导出当前格子...");
        AppendMenuW(categoryMenu, categoryOnlyState, kImportCategoryCommand, L"导入到当前格子...");
        AppendMenuW(categoryMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(categoryMenu, categoryOnlyState, kMoveCategoryUpCommand, L"分类顺序上移");
        AppendMenuW(categoryMenu, categoryOnlyState, kMoveCategoryDownCommand, L"分类顺序下移");
        AppendMenuW(utilityMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(categoryMenu), L"当前格子管理");
    }

    HMENU allWidgetsMenu = CreatePopupMenu();
    AppendMenuW(allWidgetsMenu, MF_STRING, kHideAllCommand, L"隐藏全部格子");
    AppendMenuW(allWidgetsMenu, MF_STRING, kToggleAllLockedCommand, L"切换全部格子锁定状态");
    AppendMenuW(allWidgetsMenu, MF_STRING, kRefreshAllCommand, L"刷新全部格子");
    AppendMenuW(utilityMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(allWidgetsMenu), L"全部格子");

    HMENU settingsMenu = CreatePopupMenu();
    AppendMenuW(
        settingsMenu,
        MF_STRING | (currentConfig.settings.launchOnStartup ? MF_CHECKED : 0),
        kToggleStartupCommand,
        L"开机自启");
    AppendMenuW(settingsMenu, MF_STRING, kExportConfigCommand, L"导出全部配置...");
    AppendMenuW(settingsMenu, MF_STRING, kImportConfigCommand, L"导入全部配置...");
    AppendMenuW(utilityMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(settingsMenu), L"备份与启动");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(utilityMenu), L"实用功能");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kFeedbackCommand, L"我要反馈");
    AppendMenuW(menu, MF_STRING, kSettingsCommand, L"设置中心");
    AppendMenuW(menu, MF_STRING, kPersonalCenterCommand, L"个人中心");
    AppendMenuW(menu, MF_STRING, kCheckUpdateCommand, L"检查更新");
    AppendMenuW(menu, MF_STRING, kOpenDataCommand, L"我的整理数据");
    AppendMenuW(menu, MF_STRING, kExitApplicationCommand, L"退出 Lattice");

    gActiveShellMenu2 = shellContextMenu2.Get();
    gActiveShellMenu3 = shellContextMenu3.Get();
    const int command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    gActiveShellMenu2 = nullptr;
    gActiveShellMenu3 = nullptr;
    if (command >= static_cast<int>(kShellNewCommandFirst) && command <= static_cast<int>(kShellNewCommandLast) && shellContextMenu) {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = hwnd_;
        invoke.lpVerb = MAKEINTRESOURCEA(command - kShellNewCommandFirst);
        invoke.lpVerbW = MAKEINTRESOURCEW(command - kShellNewCommandFirst);
        invoke.nShow = SW_SHOWNORMAL;
        shellContextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        DestroyMenu(menu);
        if (shellRootMenu != nullptr) {
            DestroyMenu(shellRootMenu);
        }
        RegisterUntrackedCategoryItems();
        LoadItems();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    DestroyMenu(menu);
    if (shellRootMenu != nullptr) {
        DestroyMenu(shellRootMenu);
    }
    if (command == kToggleCollapseCommand) {
        ToggleCollapsed();
    } else if (command == kFixedExpandedCommand) {
        SetFixedExpanded(!windowConfig_.fixedExpanded);
    } else if (command == kToggleLockCommand) {
        ToggleLocked();
    } else if (command == kOpenLocationCommand) {
        OpenCategoryLocation();
    } else if (command == kRenameCommand) {
        RenameCategory();
    } else if (command == kRefreshCommand) {
        LoadItems();
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (command == kPasteCommand || command == kPasteShortcutCommand) {
        PasteClipboardShortcuts();
    } else if (command == kFeedbackCommand) {
        OpenFeedbackDraft();
    } else if (command == kPersonalCenterCommand) {
        ShowPersonalCenter();
    } else if (command == kCheckUpdateCommand) {
        CheckForUpdates();
    } else if (command == kOpenDataCommand) {
        OpenDataLocation();
    } else if (command == kExitApplicationCommand && owner_ != nullptr) {
        SendMessageW(owner_, kWidgetHostCommandMessage, static_cast<WPARAM>(WidgetHostCommand::ExitApplication), 0);
    } else if (command != kDissolveCommand && command >= kNewCategoryCommand && command <= kImportConfigCommand && owner_ != nullptr) {
        const bool categoryCommand = command >= kImportUnassignedCommand && command <= kMoveCategoryDownCommand;
        if (categoryCommand) {
            SendMessageW(owner_, kWidgetActivateCategoryMessage, 0, reinterpret_cast<LPARAM>(categoryId_.c_str()));
        }
        WidgetHostCommand hostCommand = WidgetHostCommand::CreateCategory;
        if (command == kHideAllCommand) {
            hostCommand = WidgetHostCommand::HideAll;
        } else if (command == kToggleAllLockedCommand) {
            hostCommand = WidgetHostCommand::ToggleAllLocked;
        } else if (command == kRefreshAllCommand) {
            hostCommand = WidgetHostCommand::RefreshAll;
        } else if (command == kToggleStartupCommand) {
            hostCommand = WidgetHostCommand::ToggleStartup;
        } else if (command == kSettingsCommand) {
            hostCommand = WidgetHostCommand::ShowSettings;
        } else if (command == kExportConfigCommand) {
            hostCommand = WidgetHostCommand::ExportConfig;
        } else if (command == kImportConfigCommand) {
            hostCommand = WidgetHostCommand::ImportConfig;
        } else if (command == kImportUnassignedCommand) {
            hostCommand = WidgetHostCommand::ImportUnassigned;
        } else if (command == kExportCategoryCommand) {
            hostCommand = WidgetHostCommand::ExportCategory;
        } else if (command == kImportCategoryCommand) {
            hostCommand = WidgetHostCommand::ImportCategory;
        } else if (command == kMoveCategoryUpCommand) {
            hostCommand = WidgetHostCommand::MoveCategoryUp;
        } else if (command == kMoveCategoryDownCommand) {
            hostCommand = WidgetHostCommand::MoveCategoryDown;
        }
        SendMessageW(owner_, kWidgetHostCommandMessage, static_cast<WPARAM>(hostCommand), reinterpret_cast<LPARAM>(hwnd_));
    } else if (command == kIconSmallCommand) {
        SetIconSize(32);
    } else if (command == kIconMediumCommand) {
        SetIconSize(48);
    } else if (command == kIconLargeCommand) {
        SetIconSize(64);
    } else if (command == kGridModeCommand || command == kListModeCommand) {
        SetContentViewMode(command == kListModeCommand ? 1 : 0);
    } else if (command >= kSortCustomCommand && command <= kSortModifiedCommand) {
        SetSortMode(command - kSortCustomCommand);
    } else if (command == kAutoArrangeCommand) {
        SetAutoArrange(!windowConfig_.autoArrange);
    } else if (command == kDissolveCommand) {
        DissolveCategory();
    }
}

void WidgetWindow::DissolveCategory() {
    if (categoryId_ == kUncategorizedCategoryId) {
        return;
    }
    if (MessageDialog::Show(instance_,
            hwnd_,
            L"解散格子后，其中的桌面项目会移回原桌面位置。是否继续？",
            L"确认解散",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    AppConfig appConfig = configStore_.LoadAppConfig();
    const auto category = std::find_if(appConfig.categories.begin(), appConfig.categories.end(), [&](const CategoryConfig& value) {
        return value.id == categoryId_;
    });
    if (category == appConfig.categories.end()) {
        return;
    }
    const std::vector<std::wstring> itemIds = category->itemIds;
    for (const std::wstring& itemId : itemIds) {
        if (!MoveItemOut(itemId, false)) {
            MessageDialog::Show(instance_,
                hwnd_,
                L"至少一个桌面项目无法安全移回桌面。已经移出的项目不会重复显示；格子会保留，方便继续处理剩余项目。",
                L"未能完全解散格子",
                MB_OK | MB_ICONERROR);
            return;
        }
    }
    appConfig = configStore_.LoadAppConfig();
    const auto refreshedCategory = std::find_if(appConfig.categories.begin(), appConfig.categories.end(), [&](const CategoryConfig& value) {
        return value.id == categoryId_;
    });
    if (refreshedCategory == appConfig.categories.end()) {
        return;
    }
    CategoryStorageManager storageManager(configStore_, shortcutStore_);
    std::wstring storageError;
    if (!storageManager.RemoveEmpty(categoryId_, storageError)) {
        MessageDialog::Show(instance_, hwnd_, storageError.c_str(), L"未能完全解散格子", MB_OK | MB_ICONWARNING);
        return;
    }
    appConfig.categories.erase(refreshedCategory);
    if (!configStore_.SaveAppConfig(appConfig)) {
        MessageDialog::Show(instance_, hwnd_, L"桌面项目已移回桌面，但格子配置保存失败。", L"解散格子失败", MB_OK | MB_ICONERROR);
        return;
    }
    PostMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
    DestroyWindow(hwnd_);
}

void WidgetWindow::RenameCategory() {
    const auto name = InputDialog::Prompt(instance_, hwnd_, L"重命名格子", L"格子名称", categoryName_);
    if (!name.has_value() || name->empty()) {
        return;
    }
    CategoryStorageManager storageManager(configStore_, shortcutStore_);
    std::wstring errorMessage;
    if (!storageManager.Rename(categoryId_, *name, errorMessage)) {
        MessageDialog::Show(instance_, hwnd_, errorMessage.c_str(), L"重命名格子失败", MB_OK | MB_ICONWARNING);
        return;
    }
    categoryName_ = *name;
    SetWindowTextW(hwnd_, categoryName_.c_str());
    LoadItems();
    PostMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::RegisterUntrackedCategoryItems() {
    AppConfig config = configStore_.LoadAppConfig();
    const std::wstring directory = shortcutStore_.CategoryPath(CategoryStorageFolder(config, categoryId_));
    DesktopScanner scanner;
    bool changed = false;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        const std::wstring path = entry.path().wstring();
        const auto existing = std::find_if(config.items.begin(), config.items.end(), [&](const ItemConfig& item) {
            return CompareStringOrdinal(item.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (existing != config.items.end()) {
            continue;
        }
        const DesktopItem desktopItem = scanner.CreateItemFromPath(path);
        config.items.push_back(ItemConfig{desktopItem.id, path, desktopItem.displayName});
        if (categoryId_ == kUncategorizedCategoryId) {
            config.uncategorizedItemIds.push_back(desktopItem.id);
        } else {
            const auto category = std::find_if(config.categories.begin(), config.categories.end(), [&](const CategoryConfig& value) {
                return value.id == categoryId_;
            });
            if (category != config.categories.end()) {
                category->itemIds.push_back(desktopItem.id);
            }
        }
        changed = true;
    }
    if (changed) {
        configStore_.SaveAppConfig(config);
        PostMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
    }
}

RECT WidgetWindow::GridBounds() const {
    RECT client = ClientRectInDips();
    client.left += 3;
    client.right -= 3;
    client.top = kTitleHeight - 4;
    client.bottom -= 2;
    return client;
}

RECT WidgetWindow::CollapseButtonBounds() const {
    return RECT{0, 0, 24, kTitleHeight};
}

RECT WidgetWindow::LockButtonBounds() const {
    return RECT{24, 0, 48, kTitleHeight};
}

bool WidgetWindow::HitTestCollapseButton(POINT point) const {
    RECT rect = CollapseButtonBounds();
    return PtInRect(&rect, point) != FALSE;
}

bool WidgetWindow::HitTestLockButton(POINT point) const {
    RECT rect = LockButtonBounds();
    return PtInRect(&rect, point) != FALSE;
}

int WidgetWindow::HeaderButtonAt(POINT point) const {
    if (point.y < 0 || point.y >= kTitleHeight) {
        return -1;
    }
    if (HitTestCollapseButton(point)) {
        return 1;
    }
    if (HitTestLockButton(point)) {
        return 2;
    }
    const RECT client = ClientRectInDips();
    if (point.x >= client.right - 97 && point.x < client.right - 73) {
        return 3;
    }
    if (point.x >= client.right - 75 && point.x < client.right - 51) {
        return 4;
    }
    if (point.x >= client.right - 52 && point.x < client.right - 28) {
        return 5;
    }
    if (point.x >= client.right - 29 && point.x < client.right - 5) {
        return 6;
    }
    return -1;
}

void WidgetWindow::InvokeHeaderButton(int button, POINT pixelPoint) {
    if (button == 1) {
        ToggleCollapsed();
        return;
    }
    if (button == 2) {
        ToggleLocked();
        return;
    }
    if (button == 3) {
        OpenCategoryLocation();
        return;
    }
    if (button == 4) {
        SetContentViewMode(windowConfig_.contentViewMode == 0 ? 1 : 0);
        return;
    }
    POINT screenPoint = pixelPoint;
    ClientToScreen(hwnd_, &screenPoint);
    if (button == 5) {
        ShowSortMenu(screenPoint);
    } else if (button == 6) {
        ShowBackgroundMenu(screenPoint);
    }
}

LRESULT WidgetWindow::ResizeHitTest(POINT pixelPoint) const {
    if (windowConfig_.locked || windowConfig_.collapsed) {
        return HTCLIENT;
    }
    if (pixelPoint.y < DipToPixels(kTitleHeight)) {
        return HTCLIENT;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int border = DipToPixels(kResizeGrip);
    const bool left = pixelPoint.x >= client.left && pixelPoint.x < client.left + border;
    const bool right = pixelPoint.x < client.right && pixelPoint.x >= client.right - border;
    const bool bottom = pixelPoint.y < client.bottom && pixelPoint.y >= client.bottom - border;
    if (bottom && left) {
        return HTBOTTOMLEFT;
    }
    if (bottom && right) {
        return HTBOTTOMRIGHT;
    }
    if (left) {
        return HTLEFT;
    }
    if (right) {
        return HTRIGHT;
    }
    if (bottom) {
        return HTBOTTOM;
    }
    return HTCLIENT;
}

UINT WidgetWindow::WindowDpi() const {
    if (hwnd_ != nullptr) {
        const UINT dpi = GetDpiForWindow(hwnd_);
        if (dpi != 0) {
            return dpi;
        }
    }
    return static_cast<UINT>(std::max(96, windowConfig_.dpi));
}

int WidgetWindow::DipToPixels(int value) const {
    return MulDiv(value, static_cast<int>(WindowDpi()), 96);
}

int WidgetWindow::PixelsToDips(int value) const {
    return MulDiv(value, 96, static_cast<int>(WindowDpi()));
}

POINT WidgetWindow::ClientPixelsToDips(POINT point) const {
    point.x = PixelsToDips(point.x);
    point.y = PixelsToDips(point.y);
    return point;
}

RECT WidgetWindow::ClientRectInDips() const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    client.left = PixelsToDips(client.left);
    client.top = PixelsToDips(client.top);
    client.right = PixelsToDips(client.right);
    client.bottom = PixelsToDips(client.bottom);
    return client;
}

void WidgetWindow::UpdateHover(POINT point, bool nonClient) {
    TRACKMOUSEEVENT tracking{};
    tracking.cbSize = sizeof(tracking);
    tracking.dwFlags = TME_LEAVE | (nonClient ? TME_NONCLIENT : 0);
    tracking.hwndTrack = hwnd_;
    TrackMouseEvent(&tracking);

    const int nextIconIndex = windowConfig_.collapsed ? -1 : iconGrid_.HitTest(point);
    const bool nextHeaderHovered = point.y >= 0 && point.y < kTitleHeight;
    const int nextHeaderButton = nextHeaderHovered ? HeaderButtonAt(point) : -1;
    if (nextIconIndex == hoverIconIndex_ &&
        nextHeaderButton == hoverHeaderButton_ &&
        nextHeaderHovered == headerHovered_) {
        return;
    }
    hoverIconIndex_ = nextIconIndex;
    hoverHeaderButton_ = nextHeaderButton;
    headerHovered_ = nextHeaderHovered;
    iconGrid_.SetHoverIndex(nextIconIndex);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool WidgetWindow::UpdateIconDrag(POINT pixelPoint) {
    if (windowConfig_.collapsed || draggingIconIndex_ < 0) {
        return false;
    }
    const POINT point = ClientPixelsToDips(pixelPoint);
    const bool movedEnough =
        point.x - dragStartPoint_.x > 6 || dragStartPoint_.x - point.x > 6 ||
        point.y - dragStartPoint_.y > 6 || dragStartPoint_.y - point.y > 6;
    if (!movedEnough) {
        return false;
    }

    if (!dragVisualActive_) {
        const DesktopItem* movingItem = iconGrid_.ItemAt(static_cast<size_t>(draggingIconIndex_));
        const RECT sourceCell = iconGrid_.CellAt(static_cast<size_t>(draggingIconIndex_));
        if (movingItem != nullptr) {
            POINT screenPoint = pixelPoint;
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
        POINT screenPoint = pixelPoint;
        ClientToScreen(hwnd_, &screenPoint);
        DragGhostWindow::Instance().Update(screenPoint);
    }

    const RECT gridBounds = GridBounds();
    if (PtInRect(&gridBounds, point) != FALSE) {
        const int nextTarget = iconGrid_.SlotIndexForPoint(point);
        if (nextTarget != dragTargetIndex_) {
            dragTargetIndex_ = nextTarget;
            iconGrid_.SetHoverIndex(nextTarget);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return true;
    }

    const bool targetChanged = dragTargetIndex_ != -1;
    dragTargetIndex_ = -1;
    const bool hoverChanged = iconGrid_.SetHoverIndex(-1);
    if (targetChanged || hoverChanged) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return true;
}

void WidgetWindow::FinishIconDrag(POINT pixelPoint) {
    if (draggingIconIndex_ < 0) {
        KillTimer(hwnd_, kIconDragTimerId);
        draggingItemId_.clear();
        FlushDeferredRefresh();
        return;
    }

    int originIndex = draggingIconIndex_;
    if (!draggingItemId_.empty()) {
        const auto origin = std::find_if(
            currentItems_.begin(),
            currentItems_.end(),
            [&](const DesktopItem& item) {
                return item.id == draggingItemId_;
            });
        if (origin != currentItems_.end()) {
            originIndex = static_cast<int>(
                std::distance(currentItems_.begin(), origin));
        } else {
            originIndex = -1;
        }
    }
    const POINT point = ClientPixelsToDips(pixelPoint);
    const DesktopItem* movingItem =
        iconGrid_.ItemAt(static_cast<size_t>(originIndex));
    const std::wstring movingId = !draggingItemId_.empty()
        ? draggingItemId_
        : (movingItem == nullptr ? L"" : movingItem->id);
    const RECT gridBounds = GridBounds();
    const bool droppedInside = PtInRect(&gridBounds, point) != FALSE;
    POINT cursorScreenPoint = pixelPoint;
    ClientToScreen(hwnd_, &cursorScreenPoint);
    WidgetWindow* targetWidget = droppedInside
        ? nullptr
        : DropTargetWidgetAtScreenPoint(cursorScreenPoint);
    const int targetIndex = droppedInside
        ? (dragTargetIndex_ >= 0 ? dragTargetIndex_ : iconGrid_.SlotIndexForPoint(point))
        : -1;
    const bool completedDrag = dragVisualActive_;
    const std::uint64_t dragGhostGeneration =
        completedDrag ? dragGhostGeneration_ : 0;
    POINT dropScreenPoint{};
    if (completedDrag) {
        DragGhostWindow::Instance().Commit(cursorScreenPoint);
        dropScreenPoint = DragGhostWindow::Instance().TopLeftScreenPoint();
    }

    KillTimer(hwnd_, kIconDragTimerId);
    iconGrid_.SetDraggingIndex(-1);
    draggingIconIndex_ = -1;
    dragTargetIndex_ = -1;
    iconGrid_.SetHoverIndex(-1);
    dragVisualActive_ = false;
    dragGhostGeneration_ = 0;
    draggingItemId_.clear();
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    if (completedDrag) {
        if (targetWidget != nullptr &&
            targetWidget->categoryId_ != categoryId_ &&
            !movingId.empty()) {
            DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
            MoveItemToCategory(movingId, targetWidget->categoryId_);
        } else if (!droppedInside && !movingId.empty()) {
            if (!MoveItemOut(
                    movingId,
                    true,
                    &dropScreenPoint,
                    dragGhostGeneration)) {
                DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
            }
        } else if (originIndex >= 0 && targetIndex >= 0) {
            DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
            ReorderItem(static_cast<size_t>(originIndex), static_cast<size_t>(targetIndex));
        } else {
            DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
        }
    } else {
        DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    FlushDeferredRefresh();
}

void WidgetWindow::CancelIconDrag() {
    KillTimer(hwnd_, kIconDragTimerId);
    DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration_);
    dragGhostGeneration_ = 0;
    dragVisualActive_ = false;
    draggingItemId_.clear();
    iconGrid_.SetDraggingIndex(-1);
    draggingIconIndex_ = -1;
    dragTargetIndex_ = -1;
    iconGrid_.SetHoverIndex(-1);
    if (GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    FlushDeferredRefresh();
}

void WidgetWindow::FlushDeferredRefresh() {
    if (!refreshPending_) {
        return;
    }
    refreshPending_ = false;
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) {
        PostMessageW(hwnd_, kWidgetRefreshMessage, 0, 0);
    }
}

DesktopItem* WidgetWindow::FindItem(const std::wstring& itemId) {
    for (DesktopItem& item : items_) {
        if (item.id == itemId) {
            return &item;
        }
    }
    return nullptr;
}

void WidgetWindow::ShowIconMenu(POINT screenPoint, int iconIndex) {
    const DesktopItem* item = iconGrid_.ItemAt(static_cast<size_t>(iconIndex));
    if (item == nullptr) {
        return;
    }

    constexpr int kOpenCommand = 11;
    constexpr int kShowCommand = 12;
    constexpr int kAdminCommand = 13;
    constexpr int kRenameCommand = 14;
    constexpr int kRefreshCommand = 15;
    constexpr int kMoveOutCommand = 16;
    constexpr int kMoveToUncategorizedCommand = 17;
    constexpr int kMoveCategoryBaseCommand = 1000;
    const AppConfig appConfig = configStore_.LoadAppConfig();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpenCommand, L"\u6253\u5f00");
    AppendMenuW(menu, MF_STRING, kShowCommand, L"\u6253\u5f00\u6240\u5728\u4f4d\u7f6e");
    AppendMenuW(menu, MF_STRING, kAdminCommand, L"\u4ee5\u7ba1\u7406\u5458\u8fd0\u884c");
    AppendMenuW(menu, MF_STRING, kRenameCommand, L"\u91cd\u547d\u540d\u663e\u793a\u540d");
    AppendMenuW(menu, MF_STRING, kRefreshCommand, L"\u5237\u65b0\u56fe\u6807");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU moveMenu = CreatePopupMenu();
    if (categoryId_ != kUncategorizedCategoryId) {
        AppendMenuW(moveMenu, MF_STRING, kMoveToUncategorizedCommand, L"\u672a\u5206\u7c7b");
    }
    for (size_t index = 0; index < appConfig.categories.size(); ++index) {
        if (appConfig.categories[index].id != categoryId_) {
            AppendMenuW(
                moveMenu,
                MF_STRING,
                kMoveCategoryBaseCommand + static_cast<UINT>(index),
                appConfig.categories[index].name.c_str());
        }
    }
    if (GetMenuItemCount(moveMenu) == 0) {
        AppendMenuW(moveMenu, MF_GRAYED, 0, L"\u6682\u65e0\u5176\u4ed6\u5206\u7c7b");
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveMenu), L"\u79fb\u52a8\u5230\u5206\u7c7b");
    AppendMenuW(menu, MF_STRING, kMoveOutCommand, L"移出格子");
    const int command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (command == kOpenCommand) {
        launcher_.OpenPath(item->path);
    } else if (command == kShowCommand) {
        launcher_.ShowInExplorer(item->path);
    } else if (command == kAdminCommand) {
        if (MessageDialog::Show(instance_,
                hwnd_,
                L"\u5c06\u4ee5\u7ba1\u7406\u5458\u6743\u9650\u8fd0\u884c\u8be5\u9879\u76ee\uff1f",
                L"\u9700\u8981\u786e\u8ba4",
                MB_YESNO | MB_ICONWARNING) == IDYES) {
            launcher_.RunAsAdministrator(item->path);
        }
    } else if (command == kRenameCommand) {
        const auto name = InputDialog::Prompt(instance_, hwnd_, L"\u91cd\u547d\u540d\u663e\u793a\u540d", L"\u663e\u793a\u540d", item->displayName);
        if (name.has_value() && !name->empty()) {
            AppConfig updatedConfig = configStore_.LoadAppConfig();
            for (ItemConfig& registered : updatedConfig.items) {
                if (registered.id == item->id) {
                    registered.displayName = *name;
                    break;
                }
            }
            configStore_.SaveAppConfig(updatedConfig);
            LoadItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
            PostMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
        }
    } else if (command == kRefreshCommand) {
        RefreshIconCache();
    } else if (command == kMoveOutCommand) {
        MoveItemOut(item->id);
    } else if (command == kMoveToUncategorizedCommand) {
        MoveItemToCategory(item->id, kUncategorizedCategoryId);
    } else if (command >= kMoveCategoryBaseCommand &&
               command < kMoveCategoryBaseCommand + static_cast<int>(appConfig.categories.size())) {
        const size_t categoryIndex = static_cast<size_t>(command - kMoveCategoryBaseCommand);
        MoveItemToCategory(item->id, appConfig.categories[categoryIndex].id);
    }
}

void WidgetWindow::MoveItemToCategory(const std::wstring& itemId, const std::wstring& targetCategoryId) {
    if (itemId.empty() || targetCategoryId.empty()) {
        return;
    }
    const DesktopItem* item = FindItem(itemId);
    if (item == nullptr) {
        return;
    }
    const std::wstring sourcePath = item->path;
    const std::wstring displayName = item->displayName;
    AppConfig savedConfig = configStore_.LoadAppConfig();
    if (targetCategoryId != kUncategorizedCategoryId &&
        std::none_of(savedConfig.categories.begin(), savedConfig.categories.end(), [&](const CategoryConfig& category) {
            return category.id == targetCategoryId;
        })) {
        return;
    }

    std::wstring destinationPath;
    std::wstring errorMessage;
    const auto persistCategoryMove = [&](const std::wstring& storedPath) {
            auto registered = std::find_if(savedConfig.items.begin(), savedConfig.items.end(), [&](const ItemConfig& value) {
                return value.id == itemId;
            });
            if (registered == savedConfig.items.end()) {
                savedConfig.items.push_back(ItemConfig{itemId, storedPath, displayName});
            } else {
                registered->path = storedPath;
            }
            savedConfig.uncategorizedItemIds.erase(
                std::remove(savedConfig.uncategorizedItemIds.begin(), savedConfig.uncategorizedItemIds.end(), itemId),
                savedConfig.uncategorizedItemIds.end());
            for (CategoryConfig& category : savedConfig.categories) {
                category.itemIds.erase(
                    std::remove(category.itemIds.begin(), category.itemIds.end(), itemId),
                    category.itemIds.end());
            }
            if (targetCategoryId == kUncategorizedCategoryId) {
                savedConfig.uncategorizedItemIds.push_back(itemId);
            } else {
                const auto category = std::find_if(savedConfig.categories.begin(), savedConfig.categories.end(), [&](const CategoryConfig& value) {
                    return value.id == targetCategoryId;
                });
                if (category == savedConfig.categories.end()) {
                    return false;
                }
                category->itemIds.push_back(itemId);
            }
            return configStore_.SaveAppConfig(savedConfig);
        };
    const bool requiresPhysicalMove =
        shortcutStore_.RequiresManagedStorage(sourcePath);
    bool moved = false;
    if (requiresPhysicalMove) {
        moved = shortcutStore_.MoveIntoCategory(
            itemId,
            sourcePath,
            CategoryStorageFolder(savedConfig, targetCategoryId),
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
        MessageDialog::Show(instance_, hwnd_, errorMessage.c_str(), L"移动桌面项目失败", MB_OK | MB_ICONERROR);
        return;
    }
    LoadItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (owner_ != nullptr) {
        PostMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
    }
}

WidgetWindow* WidgetWindow::DropTargetWidgetAtScreenPoint(POINT screenPoint) const {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd_, &processId);
    for (HWND candidate = GetTopWindow(nullptr);
         candidate != nullptr;
         candidate = GetWindow(candidate, GW_HWNDNEXT)) {
        if (candidate == hwnd_ || IsWindowVisible(candidate) == FALSE) {
            continue;
        }
        DWORD candidateProcessId = 0;
        GetWindowThreadProcessId(candidate, &candidateProcessId);
        if (candidateProcessId != processId) {
            continue;
        }
        wchar_t className[64]{};
        if (GetClassNameW(candidate, className, ARRAYSIZE(className)) == 0 ||
            wcscmp(className, kWindowClassName) != 0) {
            continue;
        }
        RECT windowRect{};
        if (!GetWindowRect(candidate, &windowRect) || PtInRect(&windowRect, screenPoint) == FALSE) {
            continue;
        }
        auto* target = reinterpret_cast<WidgetWindow*>(GetWindowLongPtrW(candidate, GWLP_USERDATA));
        if (target == nullptr || target->windowConfig_.collapsed) {
            continue;
        }
        POINT clientPoint = screenPoint;
        ScreenToClient(candidate, &clientPoint);
        const POINT dipPoint = target->ClientPixelsToDips(clientPoint);
        const RECT targetGrid = target->GridBounds();
        if (PtInRect(&targetGrid, dipPoint) != FALSE) {
            return target;
        }
    }
    return nullptr;
}

bool WidgetWindow::MoveItemOut(
    const std::wstring& itemId,
    bool showError,
    const POINT* dropScreenPoint,
    std::uint64_t dragGhostGeneration) {
    const DesktopItem* item = FindItem(itemId);
    if (item == nullptr) {
        return false;
    }
    const std::wstring sourcePath = item->path;
    const AppConfig beforeRemoval = configStore_.LoadAppConfig();
    const auto registeredBeforeRemoval = std::find_if(beforeRemoval.items.begin(), beforeRemoval.items.end(), [&](const ItemConfig& value) {
        return value.id == itemId;
    });
    ItemConfig placement = registeredBeforeRemoval == beforeRemoval.items.end() ? ItemConfig{} : *registeredBeforeRemoval;
    const auto removeFromConfig = [&]() {
        AppConfig config = configStore_.LoadAppConfig();
        config.uncategorizedItemIds.erase(
            std::remove(config.uncategorizedItemIds.begin(), config.uncategorizedItemIds.end(), itemId),
            config.uncategorizedItemIds.end());
        for (CategoryConfig& category : config.categories) {
            category.itemIds.erase(
                std::remove(category.itemIds.begin(), category.itemIds.end(), itemId),
                category.itemIds.end());
        }
        config.items.erase(
            std::remove_if(config.items.begin(), config.items.end(), [&](const ItemConfig& value) {
                return value.id == itemId;
            }),
            config.items.end());
        return configStore_.SaveAppConfig(config);
    };

    bool moved = false;
    std::wstring desktopPath = sourcePath;
    std::wstring errorMessage;
    if (!shortcutStore_.IsManagedPath(sourcePath)) {
        bool restoredVisibility = true;
        if (placement.desktopVisibilityMode != 0) {
            restoredVisibility = shortcutStore_.RestoreDesktopVisibility(
                placement,
                errorMessage,
                dropScreenPoint);
        }
        moved = restoredVisibility && removeFromConfig();
        if (!moved) {
            if (restoredVisibility) {
                std::wstring rollbackError;
                shortcutStore_.SuppressDesktopVisibility(placement, rollbackError);
                errorMessage = L"项目原件未发生改变，但无法保存移出格子的配置。";
            }
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
            [&](const std::wstring&) { return removeFromConfig(); },
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
        DragGhostWindow::Instance().EndIfGeneration(dragGhostGeneration);
        if (showError) {
            MessageDialog::Show(instance_, hwnd_, errorMessage.c_str(), L"移出格子失败", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    if (owner_ != nullptr && IsWindow(owner_) != FALSE) {
        PostMessageW(owner_, kOrganizerConfigSyncMessage, 0, 0);
    }
    bool placementQueued = false;
    const bool canPlaceOnDesktop = shortcutStore_.IsDesktopPath(desktopPath);
    if (dropScreenPoint != nullptr && canPlaceOnDesktop) {
        DesktopPlacementRequest request;
        request.path = desktopPath;
        request.screenPoint = *dropScreenPoint;
        request.dragGhostGeneration = dragGhostGeneration;
        request.showError = showError;
        request.sourceWindow = hwnd_;
        placementQueued =
            owner_ != nullptr &&
            IsWindow(owner_) != FALSE &&
            SendMessageW(
                owner_,
                kDesktopPlacementRequestMessage,
                0,
                reinterpret_cast<LPARAM>(&request)) != 0;
        if (!placementQueued) {
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
                AppConfig updatedConfig = configStore_.LoadAppConfig();
                auto savedPosition = std::find_if(
                    updatedConfig.desktopLayout.begin(),
                    updatedConfig.desktopLayout.end(),
                    [&](const DesktopPlacementConfig& value) {
                        return CompareStringOrdinal(
                                   value.path.c_str(),
                                   -1,
                                   desktopPath.c_str(),
                                   -1,
                                   TRUE) == CSTR_EQUAL;
                    });
                if (savedPosition == updatedConfig.desktopLayout.end()) {
                    updatedConfig.desktopLayout.push_back(
                        DesktopPlacementConfig{desktopPath, restoredPoint.x, restoredPoint.y});
                } else {
                    savedPosition->x = restoredPoint.x;
                    savedPosition->y = restoredPoint.y;
                }
                if (!configStore_.SaveAppConfig(updatedConfig) && showError) {
                    MessageDialog::Show(
                        instance_,
                        hwnd_,
                        L"图标已放到鼠标释放位置，但无法把新位置写入桌面布局快照。",
                        L"桌面坐标保存",
                        MB_OK | MB_ICONWARNING);
                }
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
        LoadItems();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (owner_ != nullptr && (dropScreenPoint == nullptr || !placementQueued)) {
        PostMessageW(owner_, kOrganizerConfigChangedMessage, 0, 0);
    }
    return true;
}

void WidgetWindow::RefreshWallpaperBackdrop() {
    if (hwnd_ == nullptr || d2d_.Target() == nullptr) {
        return;
    }
    wallpaperBackdrop_.Refresh(hwnd_, d2d_.Target());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WidgetWindow::ScheduleWallpaperBackdropRefresh() {
    if (hwnd_ != nullptr) {
        SetTimer(hwnd_, kBackdropRefreshTimerId, kBackdropRefreshDelayMilliseconds, nullptr);
    }
}

void WidgetWindow::Render() {
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);
    if (d2d_.Target() == nullptr) {
        d2d_.RecreateTarget(hwnd_);
    }

    d2d_.BeginDraw();
    ID2D1HwndRenderTarget* target = d2d_.Target();
    if (target != nullptr) {
        target->Clear(D2D1::ColorF(0x061E24, 1.0f));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> backgroundTintBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> titleShadowBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> headerHoverBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> headerPressedBrush;
        const bool lightTheme = UseLightTheme(theme_);
        target->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0xDDEFF3 : 0x082A32, lightTheme ? 0.34f : 0.68f),
            backgroundTintBrush.GetAddressOf());
        target->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x4C90A3 : 0x2E829D, lightTheme ? 0.32f : 0.18f),
            borderBrush.GetAddressOf());
        target->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x15303B : 0xFFFFFF, 1.0f), textBrush.GetAddressOf());
        target->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x15303B : 0xDAE0E2, 1.0f), iconBrush.GetAddressOf());
        target->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x15303B : 0x000000, lightTheme ? 0.18f : 0.72f), titleShadowBrush.GetAddressOf());
        target->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x79CBE2 : 0xBCEEFF, lightTheme ? 0.24f : 0.28f), headerHoverBrush.GetAddressOf());
        target->CreateSolidColorBrush(
            D2D1::ColorF(lightTheme ? 0x4AAFCB : 0x8ADDF6, lightTheme ? 0.38f : 0.42f), headerPressedBrush.GetAddressOf());

        const D2D1_SIZE_F size = target->GetSize();
        wallpaperBackdrop_.Draw(target, D2D1::RectF(0.0f, 0.0f, size.width, size.height));
        if (backgroundTintBrush != nullptr) {
            target->FillRectangle(D2D1::RectF(0.0f, 0.0f, size.width, size.height), backgroundTintBrush.Get());
        }
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        target->DrawLine(D2D1::Point2F(0.5f, 0.0f), D2D1::Point2F(0.5f, size.height), borderBrush.Get(), 1.0f);
        target->DrawLine(D2D1::Point2F(size.width - 0.5f, 0.0f), D2D1::Point2F(size.width - 0.5f, size.height), borderBrush.Get(), 1.0f);
        target->DrawLine(D2D1::Point2F(0.0f, 0.5f), D2D1::Point2F(size.width, 0.5f), borderBrush.Get(), 1.0f);
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const auto drawHeaderHover = [&](int button, const D2D1_RECT_F& rect) {
            ID2D1SolidColorBrush* stateBrush = nullptr;
            if (pressedHeaderButton_ == button && hoverHeaderButton_ == button) {
                stateBrush = headerPressedBrush.Get();
            } else if (hoverHeaderButton_ == button) {
                stateBrush = headerHoverBrush.Get();
            }
            if (stateBrush != nullptr) {
                target->FillRoundedRectangle(D2D1::RoundedRect(rect, 1.5f, 1.5f), stateBrush);
            }
        };
        const bool showHeaderControls = headerHovered_ || pressedHeaderButton_ >= 0;
        if (showHeaderControls) {
            drawHeaderHover(1, D2D1::RectF(0.0f, 7.5f + kHeaderControlOffsetY, 24.0f, 31.5f + kHeaderControlOffsetY));
            drawHeaderHover(2, D2D1::RectF(24.0f, 7.5f + kHeaderControlOffsetY, 48.0f, 31.5f + kHeaderControlOffsetY));
            drawHeaderHover(3, D2D1::RectF(size.width - 97.0f, 7.5f + kHeaderControlOffsetY, size.width - 73.0f, 31.5f + kHeaderControlOffsetY));
            drawHeaderHover(4, D2D1::RectF(size.width - 75.0f, 7.5f + kHeaderControlOffsetY, size.width - 51.0f, 31.5f + kHeaderControlOffsetY));
            drawHeaderHover(5, D2D1::RectF(size.width - 52.0f, 7.5f + kHeaderControlOffsetY, size.width - 28.0f, 31.5f + kHeaderControlOffsetY));
            drawHeaderHover(6, D2D1::RectF(size.width - 29.0f, 7.5f + kHeaderControlOffsetY, size.width - 5.0f, 31.5f + kHeaderControlOffsetY));
        }

        Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat;
        d2d_.WriteFactory()->CreateTextFormat(
            L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-cn", titleFormat.GetAddressOf());
        if (titleFormat != nullptr) {
            titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const D2D1_RECT_F titleRect = D2D1::RectF(0.0f, 0.0f, size.width, static_cast<FLOAT>(kTitleHeight));
            if (titleShadowBrush != nullptr) {
                target->DrawTextW(
                    categoryName_.c_str(),
                    static_cast<UINT32>(categoryName_.size()),
                    titleFormat.Get(),
                    D2D1::RectF(titleRect.left + 0.7f, titleRect.top + 1.1f, titleRect.right + 0.7f, titleRect.bottom + 1.1f),
                    titleShadowBrush.Get());
            }
            target->DrawTextW(categoryName_.c_str(), static_cast<UINT32>(categoryName_.size()), titleFormat.Get(), titleRect, textBrush.Get());
        }

        if (showHeaderControls) {
        const FLOAT stroke = 2.0f / 3.0f;
        auto lineWithWidth = [&](FLOAT x1, FLOAT y1, FLOAT x2, FLOAT y2, FLOAT width) {
            target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), iconBrush.Get(), width);
        };
        auto line = [&](FLOAT x1, FLOAT y1, FLOAT x2, FLOAT y2) {
            lineWithWidth(x1, y1, x2, y2, stroke);
        };
        if (windowConfig_.collapsed) {
            line(7.0f, 16.35f + kHeaderControlOffsetY, 12.0f, 21.2f + kHeaderControlOffsetY);
            line(12.0f, 21.2f + kHeaderControlOffsetY, 17.0f, 16.35f + kHeaderControlOffsetY);
        } else {
            line(7.0f, 21.2f + kHeaderControlOffsetY, 12.0f, 16.35f + kHeaderControlOffsetY);
            line(12.0f, 16.35f + kHeaderControlOffsetY, 17.0f, 21.2f + kHeaderControlOffsetY);
        }

        target->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(31.25f, 18.5f + kHeaderControlOffsetY, 40.75f, 24.5f + kHeaderControlOffsetY), 0.8f, 0.8f),
            iconBrush.Get(),
            0.8f);
        if (windowConfig_.locked) {
            lineWithWidth(32.75f, 18.5f + kHeaderControlOffsetY, 32.75f, 16.2f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(32.75f, 16.2f + kHeaderControlOffsetY, 34.35f, 14.4f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(34.35f, 14.4f + kHeaderControlOffsetY, 37.75f, 14.4f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(37.75f, 14.4f + kHeaderControlOffsetY, 39.25f, 16.2f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(39.25f, 16.2f + kHeaderControlOffsetY, 39.25f, 18.5f + kHeaderControlOffsetY, 0.8f);
        } else {
            lineWithWidth(34.75f, 18.5f + kHeaderControlOffsetY, 34.75f, 16.25f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(34.75f, 16.25f + kHeaderControlOffsetY, 36.15f, 14.55f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(36.15f, 14.55f + kHeaderControlOffsetY, 39.05f, 14.55f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(39.05f, 14.55f + kHeaderControlOffsetY, 40.65f, 16.1f + kHeaderControlOffsetY, 0.8f);
        }

        const FLOAT rightY = 20.0f + kHeaderControlOffsetY;
        const FLOAT actionLeft = size.width - 97.5f;
        const FLOAT actionTop = 7.8333f + kHeaderControlOffsetY;
        constexpr FLOAT actionCenterX = 12.20f;
        constexpr FLOAT actionCenterY = 11.575f;
        constexpr FLOAT actionScale = 0.84f;
        const auto actionX = [&](FLOAT value) { return actionLeft + actionCenterX + (value - actionCenterX) * actionScale; };
        const auto actionY = [&](FLOAT value) { return actionTop + actionCenterY + (value - actionCenterY) * actionScale; };
        const auto actionLine = [&](FLOAT x1, FLOAT y1, FLOAT x2, FLOAT y2) {
            lineWithWidth(actionX(x1), actionY(y1), actionX(x2), actionY(y2), 0.8f);
        };
        actionLine(13.77f, 5.86f, 7.49f, 5.86f);
        actionLine(7.49f, 5.86f, 6.86f, 6.49f);
        actionLine(6.86f, 6.49f, 6.86f, 16.66f);
        actionLine(6.86f, 16.66f, 7.49f, 17.29f);
        actionLine(7.49f, 17.29f, 16.29f, 17.29f);
        actionLine(16.29f, 17.29f, 16.91f, 16.66f);
        actionLine(16.91f, 16.66f, 16.91f, 10.89f);
        actionLine(8.74f, 13.40f, 12.51f, 13.40f);
        actionLine(17.54f, 6.49f, 13.77f, 10.26f);

        const FLOAT listX = size.width - 62.5f;
        for (int row = 0; row < 3; ++row) {
            const FLOAT top = 14.6667f + kHeaderControlOffsetY + static_cast<FLOAT>(row) * 4.0f;
            target->DrawRectangle(D2D1::RectF(listX - 5.5f, top, listX - 2.5f, top + 2.0f), iconBrush.Get(), 0.8f);
            lineWithWidth(listX + 0.5f, top + 1.0f, listX + 4.5f, top + 1.0f, 0.8f);
        }

        const FLOAT filterX = size.width - 40.0f;
        line(filterX - 5.5f, rightY - 5.0f, filterX + 5.5f, rightY - 5.0f);
        line(filterX - 5.5f, rightY - 5.0f, filterX - 0.8f, rightY + 0.5f);
        line(filterX + 5.5f, rightY - 5.0f, filterX + 0.8f, rightY + 0.5f);
        line(filterX + 0.8f, rightY + 0.5f, filterX + 0.8f, rightY + 4.3f);
        line(filterX + 3.5f, rightY - 1.5f, filterX + 5.5f, rightY - 1.5f);
        line(filterX + 3.5f, rightY + 1.0f, filterX + 5.2f, rightY + 1.0f);
        line(filterX + 3.5f, rightY + 3.5f, filterX + 4.9f, rightY + 3.5f);

        const FLOAT menuX = size.width - 17.0f;
        target->FillRectangle(D2D1::RectF(menuX - 5.0f, 15.3333f + kHeaderControlOffsetY, menuX + 5.0f, 16.0f + kHeaderControlOffsetY), iconBrush.Get());
        target->FillRectangle(D2D1::RectF(menuX - 5.0f, 19.3333f + kHeaderControlOffsetY, menuX + 5.0f, 20.0f + kHeaderControlOffsetY), iconBrush.Get());
        target->FillRectangle(D2D1::RectF(menuX - 5.0f, 23.3333f + kHeaderControlOffsetY, menuX + 5.0f, 24.0f + kHeaderControlOffsetY), iconBrush.Get());
        }

        if (!windowConfig_.collapsed) {
            iconGrid_.Draw(d2d_, iconCache_);
            if (shellDropPreviewActive_ &&
                shellDropInsertionIndex_ >= 0) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> insertionBrush;
                target->CreateSolidColorBrush(
                    D2D1::ColorF(0x63D3F1, 0.92f),
                    insertionBrush.GetAddressOf());
                RECT slot = iconGrid_.InsertionCellAt(
                    static_cast<size_t>(shellDropInsertionIndex_));
                const RECT gridBounds = GridBounds();
                RECT clipped{};
                if (insertionBrush != nullptr &&
                    IntersectRect(&clipped, &slot, &gridBounds) &&
                    clipped.right > clipped.left &&
                    clipped.bottom > clipped.top) {
                    const D2D1_RECT_F marker = D2D1::RectF(
                        static_cast<FLOAT>(clipped.left) + 3.0f,
                        static_cast<FLOAT>(clipped.top) + 3.0f,
                        static_cast<FLOAT>(clipped.right) - 3.0f,
                        static_cast<FLOAT>(clipped.bottom) - 3.0f);
                    if (marker.right > marker.left &&
                        marker.bottom > marker.top) {
                        target->DrawRoundedRectangle(
                            D2D1::RoundedRect(marker, 5.0f, 5.0f),
                            insertionBrush.Get(),
                            2.0f);
                    }
                }
            }
        }
    }
    const HRESULT hr = d2d_.EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        iconCache_.Clear();
        d2d_.RecreateTarget(hwnd_);
        RefreshWallpaperBackdrop();
    }
    EndPaint(hwnd_, &paint);
}
