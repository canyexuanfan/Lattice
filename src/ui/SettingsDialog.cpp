#include "ui/SettingsDialog.h"

#include <CommCtrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>

namespace {

constexpr wchar_t kClassName[] = L"Lattice.SettingsDialog";
constexpr int kLaunchOnStartupId = 1001;
constexpr int kShowPublicId = 1002;
constexpr int kRestoreHiddenId = 1003;
constexpr int kSingleClickId = 1004;
constexpr int kThemeId = 1005;
constexpr int kBackupCountId = 1006;
constexpr int kOkId = 1007;
constexpr int kCancelId = 1008;
constexpr int kStartHiddenId = 1009;
constexpr int kIconCacheSizeId = 1010;
constexpr COLORREF kDialogBackground = RGB(7, 37, 48);
constexpr COLORREF kDialogPanel = RGB(12, 49, 62);
constexpr COLORREF kDialogPanelHover = RGB(24, 65, 80);
constexpr COLORREF kDialogBorder = RGB(66, 124, 143);
constexpr COLORREF kDialogAccent = RGB(79, 171, 204);
constexpr COLORREF kDialogText = RGB(244, 249, 251);
constexpr COLORREF kDialogMutedText = RGB(157, 193, 205);
constexpr UINT kButtonHoverMessage = WM_APP + 43;
constexpr UINT kThemeHoverMessage = WM_APP + 44;

HBRUSH DialogBackgroundBrush() {
    static HBRUSH brush = CreateSolidBrush(kDialogBackground);
    return brush;
}

HBRUSH DialogPanelBrush() {
    static HBRUSH brush = CreateSolidBrush(kDialogPanel);
    return brush;
}

struct State {
    HINSTANCE instance = nullptr;
    HWND owner = nullptr;
    HWND launchOnStartup = nullptr;
    HWND showPublic = nullptr;
    HWND restoreHidden = nullptr;
    HWND singleClick = nullptr;
    HWND startHidden = nullptr;
    HWND theme = nullptr;
    HWND backupCount = nullptr;
    HWND iconCacheSize = nullptr;
    AppSettings* settings = nullptr;
    bool accepted = false;
    HWND ok = nullptr;
    HWND cancel = nullptr;
    HFONT titleFont = nullptr;
    HFONT textFont = nullptr;
    UINT dpi = 96;
    int width = 420;
    int height = 420;
    int hoverButton = 0;
    bool themeHovered = false;
    bool closeHovered = false;
    bool closePressed = false;
};

class ScopedPerMonitorV2Awareness {
public:
    ScopedPerMonitorV2Awareness() {
        previous_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~ScopedPerMonitorV2Awareness() {
        if (previous_ != nullptr) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }
private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

int Scale(const State& state, int value) {
    return MulDiv(value, static_cast<int>(state.dpi), 96);
}

RECT CloseBounds(const State& state) {
    return RECT{
        state.width - Scale(state, 36),
        Scale(state, 8),
        state.width - Scale(state, 8),
        Scale(state, 36)};
}

void SetFont(State* state, HWND control) {
    if (control != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->textFont), TRUE);
        SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
    }
}

HWND CreateLabel(State* state, const wchar_t* text, int x, int y, int width, int height) {
    HWND label = CreateWindowW(
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE,
        Scale(*state, x),
        Scale(*state, y),
        Scale(*state, width),
        Scale(*state, height),
        state->owner,
        nullptr,
        state->instance,
        nullptr);
    SetFont(state, label);
    return label;
}

HWND CreateCheckBox(State* state, const wchar_t* text, int id, int x, int y, bool checked) {
    HWND checkbox = CreateWindowW(
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        Scale(*state, x),
        Scale(*state, y),
        Scale(*state, 330),
        Scale(*state, 26),
        state->owner,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        state->instance,
        nullptr);
    SetWindowLongPtrW(checkbox, GWLP_USERDATA, checked ? 1 : 0);
    SetFont(state, checkbox);
    return checkbox;
}

bool CheckBoxChecked(HWND checkbox) {
    return GetWindowLongPtrW(checkbox, GWLP_USERDATA) != 0;
}

void SetCheckBoxChecked(HWND checkbox, bool checked) {
    SetWindowLongPtrW(checkbox, GWLP_USERDATA, checked ? 1 : 0);
    InvalidateRect(checkbox, nullptr, FALSE);
}

bool IsCheckBoxId(UINT id) {
    return id == kLaunchOnStartupId ||
           id == kShowPublicId ||
           id == kRestoreHiddenId ||
           id == kSingleClickId ||
           id == kStartHiddenId;
}

LRESULT CALLBACK ButtonSubclass(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR id,
    DWORD_PTR) {
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tracking);
        SendMessageW(GetParent(hwnd), kButtonHoverMessage, id, 0);
    } else if (message == WM_MOUSELEAVE) {
        SendMessageW(GetParent(hwnd), kButtonHoverMessage, 0, 0);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ButtonSubclass, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void DrawButton(const DRAWITEMSTRUCT& draw, const State& state) {
    if (IsCheckBoxId(draw.CtlID)) {
        const bool hovered = state.hoverButton == static_cast<int>(draw.CtlID);
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const bool focused = (draw.itemState & ODS_FOCUS) != 0;
        const bool checked = CheckBoxChecked(draw.hwndItem);
        if (hovered || pressed) {
            HBRUSH hoverBrush = CreateSolidBrush(pressed ? RGB(25, 69, 84) : kDialogPanelHover);
            FillRect(draw.hDC, &draw.rcItem, hoverBrush);
            DeleteObject(hoverBrush);
        } else {
            FillRect(draw.hDC, &draw.rcItem, DialogBackgroundBrush());
        }

        RECT box{
            draw.rcItem.left + Scale(state, 1),
            draw.rcItem.top + Scale(state, 4),
            draw.rcItem.left + Scale(state, 19),
            draw.rcItem.top + Scale(state, 22)};
        HBRUSH boxBrush = CreateSolidBrush(checked ? kDialogAccent : kDialogPanel);
        HPEN boxPen = CreatePen(PS_SOLID, Scale(state, 1), hovered || focused || checked ? kDialogAccent : kDialogBorder);
        HGDIOBJ oldBrush = SelectObject(draw.hDC, boxBrush);
        HGDIOBJ oldPen = SelectObject(draw.hDC, boxPen);
        Rectangle(draw.hDC, box.left, box.top, box.right, box.bottom);
        SelectObject(draw.hDC, oldBrush);
        SelectObject(draw.hDC, oldPen);
        DeleteObject(boxBrush);
        DeleteObject(boxPen);

        if (checked) {
            HPEN checkPen = CreatePen(PS_SOLID, std::max(1, Scale(state, 2)), kDialogBackground);
            oldPen = SelectObject(draw.hDC, checkPen);
            MoveToEx(draw.hDC, box.left + Scale(state, 4), box.top + Scale(state, 9), nullptr);
            LineTo(draw.hDC, box.left + Scale(state, 8), box.top + Scale(state, 13));
            LineTo(draw.hDC, box.left + Scale(state, 15), box.top + Scale(state, 5));
            SelectObject(draw.hDC, oldPen);
            DeleteObject(checkPen);
        }

        wchar_t label[128]{};
        GetWindowTextW(draw.hwndItem, label, ARRAYSIZE(label));
        RECT textRect{
            draw.rcItem.left + Scale(state, 28),
            draw.rcItem.top,
            draw.rcItem.right,
            draw.rcItem.bottom};
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, kDialogText);
        SelectObject(draw.hDC, state.textFont);
        DrawTextW(draw.hDC, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const bool primary = draw.CtlID == kOkId;
    const bool hovered = state.hoverButton == static_cast<int>(draw.CtlID);
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const COLORREF fill = primary
        ? (pressed ? RGB(45, 126, 157) : hovered ? RGB(61, 151, 184) : RGB(48, 134, 166))
        : (pressed ? RGB(25, 69, 84) : hovered ? kDialogPanelHover : kDialogPanel);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, hovered || focused || primary ? kDialogAccent : kDialogBorder);
    HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
    HGDIOBJ oldPen = SelectObject(draw.hDC, pen);
    RoundRect(
        draw.hDC,
        draw.rcItem.left,
        draw.rcItem.top,
        draw.rcItem.right,
        draw.rcItem.bottom,
        Scale(state, 6),
        Scale(state, 6));
    SelectObject(draw.hDC, oldBrush);
    SelectObject(draw.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, kDialogText);
    SelectObject(draw.hDC, state.textFont);
    wchar_t label[32]{};
    GetWindowTextW(draw.hwndItem, label, ARRAYSIZE(label));
    RECT bounds = draw.rcItem;
    DrawTextW(draw.hDC, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawThemeCombo(HWND hwnd, HDC dc, const State& state) {
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    HBRUSH background = CreateSolidBrush(state.themeHovered ? kDialogPanelHover : kDialogPanel);
    FillRect(dc, &bounds, background);
    DeleteObject(background);

    HPEN border = CreatePen(PS_SOLID, 1, state.themeHovered || GetFocus() == hwnd ? kDialogAccent : kDialogBorder);
    HGDIOBJ oldPen = SelectObject(dc, border);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, 0, 0, bounds.right, bounds.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    wchar_t text[64]{};
    const LRESULT selected = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
    if (selected != CB_ERR) {
        SendMessageW(hwnd, CB_GETLBTEXT, static_cast<WPARAM>(selected), reinterpret_cast<LPARAM>(text));
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kDialogText);
    SelectObject(dc, state.textFont);
    RECT textRect{
        Scale(state, 8),
        0,
        bounds.right - Scale(state, 34),
        bounds.bottom};
    DrawTextW(dc, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const int centerX = bounds.right - Scale(state, 16);
    const int centerY = bounds.bottom / 2;
    HPEN arrow = CreatePen(PS_SOLID, std::max(1, Scale(state, 1)), state.themeHovered ? kDialogText : kDialogMutedText);
    oldPen = SelectObject(dc, arrow);
    MoveToEx(dc, centerX - Scale(state, 5), centerY - Scale(state, 2), nullptr);
    LineTo(dc, centerX, centerY + Scale(state, 3));
    LineTo(dc, centerX + Scale(state, 5), centerY - Scale(state, 2));
    SelectObject(dc, oldPen);
    DeleteObject(arrow);
}

void DrawThemeItem(const DRAWITEMSTRUCT& draw, const State& state) {
    if (draw.itemID == static_cast<UINT>(-1)) {
        return;
    }
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    HBRUSH background = CreateSolidBrush(selected ? kDialogPanelHover : kDialogPanel);
    FillRect(draw.hDC, &draw.rcItem, background);
    DeleteObject(background);
    wchar_t text[64]{};
    SendMessageW(draw.hwndItem, CB_GETLBTEXT, draw.itemID, reinterpret_cast<LPARAM>(text));
    RECT textRect = draw.rcItem;
    textRect.left += Scale(state, 8);
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, kDialogText);
    SelectObject(draw.hDC, state.textFont);
    DrawTextW(draw.hDC, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK ThemeComboSubclass(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR id,
    DWORD_PTR refData) {
    auto* state = reinterpret_cast<State*>(refData);
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tracking);
        SendMessageW(GetParent(hwnd), kThemeHoverMessage, TRUE, 0);
    } else if (message == WM_MOUSELEAVE) {
        SendMessageW(GetParent(hwnd), kThemeHoverMessage, FALSE, 0);
    } else if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        DrawThemeCombo(hwnd, dc, *state);
        EndPaint(hwnd, &paint);
        return 0;
    } else if (message == WM_PRINTCLIENT) {
        DrawThemeCombo(hwnd, reinterpret_cast<HDC>(wParam), *state);
        return 0;
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ThemeComboSubclass, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    State* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<State*>(create->lpCreateParams);
        state->owner = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    if (state == nullptr) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
        case WM_CREATE: {
            const BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
            const DWORD cornerPreference = 2;
            DwmSetWindowAttribute(hwnd, 33, &cornerPreference, sizeof(cornerPreference));
            state->titleFont = CreateFontW(
                Scale(*state, -18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            state->textFont = CreateFontW(
                Scale(*state, -15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            CreateLabel(state, L"启动与行为", 22, 58, 300, 24);
            state->launchOnStartup = CreateCheckBox(state, L"随 Windows 启动", kLaunchOnStartupId, 22, 88, state->settings->launchOnStartup);
            state->showPublic = CreateCheckBox(state, L"显示公共桌面项目", kShowPublicId, 22, 118, state->settings->showPublicDesktopItems);
            state->restoreHidden = CreateCheckBox(state, L"启动时恢复上次显示/隐藏状态", kRestoreHiddenId, 22, 148, state->settings->restoreHiddenState);
            state->singleClick = CreateCheckBox(state, L"单击打开项目（默认双击）", kSingleClickId, 22, 178, state->settings->singleClickOpen);
            state->startHidden = CreateCheckBox(state, L"启动时隐藏主窗口", kStartHiddenId, 22, 208, state->settings->startHidden);
            SetWindowSubclass(state->launchOnStartup, ButtonSubclass, kLaunchOnStartupId, 0);
            SetWindowSubclass(state->showPublic, ButtonSubclass, kShowPublicId, 0);
            SetWindowSubclass(state->restoreHidden, ButtonSubclass, kRestoreHiddenId, 0);
            SetWindowSubclass(state->singleClick, ButtonSubclass, kSingleClickId, 0);
            SetWindowSubclass(state->startHidden, ButtonSubclass, kStartHiddenId, 0);
            CreateLabel(state, L"主题", 22, 250, 80, 24);
            state->theme = CreateWindowW(
                L"COMBOBOX",
                L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                Scale(*state, 102),
                Scale(*state, 246),
                Scale(*state, 210),
                Scale(*state, 120),
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kThemeId)),
                state->instance,
                nullptr);
            SetFont(state, state->theme);
            SendMessageW(state->theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"深色"));
            SendMessageW(state->theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"浅色"));
            SendMessageW(state->theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"跟随系统"));
            SendMessageW(state->theme, CB_SETCURSEL, std::clamp(state->settings->theme, 0, 2), 0);
            SendMessageW(state->theme, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Scale(*state, 24));
            SendMessageW(state->theme, CB_SETITEMHEIGHT, 0, Scale(*state, 26));
            SetWindowSubclass(
                state->theme,
                ThemeComboSubclass,
                kThemeId,
                reinterpret_cast<DWORD_PTR>(state));
            CreateLabel(state, L"配置备份数量", 22, 288, 100, 24);
            state->backupCount = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                std::to_wstring(std::clamp(state->settings->backupCount, 1, 10)).c_str(),
                WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
                Scale(*state, 130),
                Scale(*state, 286),
                Scale(*state, 54),
                Scale(*state, 24),
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBackupCountId)),
                state->instance,
                nullptr);
            SetFont(state, state->backupCount);
            CreateLabel(state, L"\u56fe\u6807\u7f13\u5b58\u6570\u91cf", 22, 324, 100, 24);
            state->iconCacheSize = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                std::to_wstring(std::clamp(state->settings->iconCacheSize, 64, 4096)).c_str(),
                WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER,
                Scale(*state, 130),
                Scale(*state, 322),
                Scale(*state, 72),
                Scale(*state, 24),
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIconCacheSizeId)),
                state->instance,
                nullptr);
            SetFont(state, state->iconCacheSize);
            state->ok = CreateWindowW(
                L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
                Scale(*state, 208), Scale(*state, 370), Scale(*state, 86), Scale(*state, 32),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOkId)), state->instance, nullptr);
            state->cancel = CreateWindowW(
                L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                Scale(*state, 302), Scale(*state, 370), Scale(*state, 86), Scale(*state, 32),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), state->instance, nullptr);
            SetFont(state, state->ok);
            SetFont(state, state->cancel);
            SetWindowSubclass(state->ok, ButtonSubclass, kOkId, 0);
            SetWindowSubclass(state->cancel, ButtonSubclass, kCancelId, 0);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            const RECT close = CloseBounds(*state);
            if (point.y >= 0 && point.y < Scale(*state, 46) && !PtInRect(&close, point)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT close = CloseBounds(*state);
            const bool hovered = PtInRect(&close, point) != FALSE;
            if (hovered != state->closeHovered) {
                state->closeHovered = hovered;
                InvalidateRect(hwnd, &close, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            state->closeHovered = false;
            state->closePressed = false;
            const RECT close = CloseBounds(*state);
            InvalidateRect(hwnd, &close, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT close = CloseBounds(*state);
            if (PtInRect(&close, point)) {
                state->closePressed = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &close, FALSE);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (state->closePressed) {
                const RECT close = CloseBounds(*state);
                state->closePressed = false;
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                if (PtInRect(&close, point)) {
                    DestroyWindow(hwnd);
                } else {
                    InvalidateRect(hwnd, &close, FALSE);
                }
                return 0;
            }
            break;
        }

        case kButtonHoverMessage:
            state->hoverButton = static_cast<int>(wParam);
            InvalidateRect(state->launchOnStartup, nullptr, FALSE);
            InvalidateRect(state->showPublic, nullptr, FALSE);
            InvalidateRect(state->restoreHidden, nullptr, FALSE);
            InvalidateRect(state->singleClick, nullptr, FALSE);
            InvalidateRect(state->startHidden, nullptr, FALSE);
            InvalidateRect(state->ok, nullptr, FALSE);
            InvalidateRect(state->cancel, nullptr, FALSE);
            return 0;

        case kThemeHoverMessage:
            state->themeHovered = wParam != FALSE;
            InvalidateRect(state->theme, nullptr, FALSE);
            return 0;

        case WM_DRAWITEM: {
            const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw.CtlID == kThemeId) {
                DrawThemeItem(draw, *state);
            } else {
                DrawButton(draw, *state);
            }
            return TRUE;
        }

        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure->CtlID == kThemeId) {
                measure->itemHeight = Scale(*state, 26);
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kDialogText);
            SetBkColor(dc, kDialogBackground);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(DialogBackgroundBrush());
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kDialogText);
            SetBkColor(dc, kDialogPanel);
            return reinterpret_cast<LRESULT>(DialogPanelBrush());
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_COMMAND: {
            const int command = LOWORD(wParam);
            if (IsCheckBoxId(static_cast<UINT>(command)) && HIWORD(wParam) == BN_CLICKED) {
                HWND checkbox = reinterpret_cast<HWND>(lParam);
                SetCheckBoxChecked(checkbox, !CheckBoxChecked(checkbox));
                return 0;
            }
            if (command == kThemeId && HIWORD(wParam) == CBN_SELCHANGE) {
                InvalidateRect(state->theme, nullptr, FALSE);
                return 0;
            }
            if (command == kOkId) {
                state->settings->launchOnStartup = CheckBoxChecked(state->launchOnStartup);
                state->settings->showPublicDesktopItems = CheckBoxChecked(state->showPublic);
                state->settings->restoreHiddenState = CheckBoxChecked(state->restoreHidden);
                state->settings->singleClickOpen = CheckBoxChecked(state->singleClick);
                state->settings->startHidden = CheckBoxChecked(state->startHidden);
                state->settings->theme = static_cast<int>(SendMessageW(state->theme, CB_GETCURSEL, 0, 0));
                wchar_t backupText[16]{};
                GetWindowTextW(state->backupCount, backupText, ARRAYSIZE(backupText));
                try {
                    state->settings->backupCount = std::clamp(std::stoi(backupText), 1, 10);
                } catch (...) {
                    state->settings->backupCount = 3;
                }
                wchar_t iconCacheText[16]{};
                GetWindowTextW(state->iconCacheSize, iconCacheText, ARRAYSIZE(iconCacheText));
                try {
                    state->settings->iconCacheSize = std::clamp(std::stoi(iconCacheText), 64, 4096);
                } catch (...) {
                    state->settings->iconCacheSize = 256;
                }
                state->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (command == kCancelId) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, DialogBackgroundBrush());
            HPEN border = CreatePen(PS_SOLID, 1, kDialogBorder);
            HGDIOBJ oldPen = SelectObject(dc, border);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, 0, 0, client.right, client.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(border);

            const int titleHeight = Scale(*state, 46);
            HPEN divider = CreatePen(PS_SOLID, 1, RGB(25, 69, 84));
            oldPen = SelectObject(dc, divider);
            MoveToEx(dc, 0, titleHeight, nullptr);
            LineTo(dc, client.right, titleHeight);
            SelectObject(dc, oldPen);
            DeleteObject(divider);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kDialogText);
            SelectObject(dc, state->titleFont);
            RECT title{
                Scale(*state, 18),
                Scale(*state, 9),
                state->width - Scale(*state, 48),
                Scale(*state, 38)};
            DrawTextW(dc, L"设置中心", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT close = CloseBounds(*state);
            if (state->closeHovered || state->closePressed) {
                HBRUSH closeBrush = CreateSolidBrush(
                    state->closePressed ? RGB(33, 79, 94) : kDialogPanelHover);
                FillRect(dc, &close, closeBrush);
                DeleteObject(closeBrush);
            }
            SetTextColor(dc, state->closeHovered ? kDialogText : kDialogMutedText);
            DrawTextW(dc, L"×", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            DeleteObject(state->titleFont);
            DeleteObject(state->textFont);
            return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

bool SettingsDialog::Show(HINSTANCE instance, HWND owner, AppSettings& settings) {
    ScopedPerMonitorV2Awareness dpiAwareness;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = DialogBackgroundBrush();
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    State state;
    state.instance = instance;
    state.settings = &settings;
    state.dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (state.dpi < 96) {
        state.dpi = 96;
    }
    state.width = Scale(state, 420);
    state.height = Scale(state, 420);
    HWND dialog = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kClassName,
        L"Lattice 设置",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        state.width,
        state.height,
        owner,
        nullptr,
        instance,
        &state);
    if (dialog == nullptr) {
        return false;
    }

    RECT ownerRect{};
    if (owner == nullptr || !GetWindowRect(owner, &ownerRect)) {
        HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            ownerRect = monitorInfo.rcWork;
        }
    }
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - state.width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - state.height) / 2;
    SetWindowPos(dialog, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(dialog);
    const bool ownerEnabled = owner != nullptr && IsWindowEnabled(owner);
    if (ownerEnabled) {
        EnableWindow(owner, FALSE);
    }

    MSG message{};
    bool quitRequested = false;
    int quitCode = 0;
    while (IsWindow(dialog)) {
        const BOOL messageResult = GetMessageW(&message, nullptr, 0, 0);
        if (messageResult <= 0) {
            if (messageResult == 0) {
                quitRequested = true;
                quitCode = static_cast<int>(message.wParam);
            }
            break;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (quitRequested && IsWindow(dialog)) {
        DestroyWindow(dialog);
    }
    if (ownerEnabled && !quitRequested && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (quitRequested) {
        PostQuitMessage(quitCode);
    }
    return state.accepted;
}
