#include "ui/SettingsDialog.h"
#include "ui/DialogStyle.h"
#include "ui/MessageDialog.h"

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
constexpr int kDisplayModeId = 1011;
constexpr int kQuickHideId = 1012;
constexpr int kTemporaryAccessId = 1013;
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
    int* displayMode = nullptr;
    HWND displayCombo = nullptr;
    HWND quickHide = nullptr;
    HWND temporaryAccess = nullptr;
    int keys[2]{};
    int modifiers[2]{};
    bool hotkeyHovered[2]{};
    std::function<bool(const AppSettings&,std::wstring&)> validateAccess;
    std::wstring accessError;
    HWND tooltip = nullptr;
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

    lattice::ui::DrawDialogButton(draw, state.textFont, state.dpi,
        state.hoverButton == static_cast<int>(draw.CtlID), draw.CtlID == kOkId);
}

void DrawThemeCombo(HWND hwnd, HDC dc, const State& state) {
    lattice::ui::DrawDialogCombo(hwnd, dc, state.textFont, state.dpi, state.themeHovered);
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

std::wstring HotkeyLabel(int key,int modifiers) {
    if(!key)return L"已停用";
    std::wstring label;
    if(modifiers&MOD_CONTROL)label+=L"Ctrl + ";if(modifiers&MOD_ALT)label+=L"Alt + ";
    if(modifiers&MOD_SHIFT)label+=L"Shift + ";if(modifiers&MOD_WIN)label+=L"Win + ";
    if(key>=VK_F1&&key<=VK_F24)return label+L"F"+std::to_wstring(key-VK_F1+1);
    wchar_t name[80]{};const UINT scan=MapVirtualKeyW(static_cast<UINT>(key),MAPVK_VK_TO_VSC);
    GetKeyNameTextW(static_cast<LONG>(scan<<16),name,ARRAYSIZE(name));
    label+=name[0]?std::wstring(name):std::to_wstring(key);return label;
}
LRESULT CALLBACK HotkeyEditSubclass(HWND h,UINT message,WPARAM key,LPARAM param,UINT_PTR id,DWORD_PTR reference) {
    auto* state=reinterpret_cast<State*>(reference);const int role=id==kQuickHideId?0:1;
    if(message==WM_GETDLGCODE)return DLGC_WANTALLKEYS;
    if(message==WM_KEYDOWN || message==WM_SYSKEYDOWN) {
        if(key==VK_SHIFT||key==VK_CONTROL||key==VK_MENU||key==VK_LWIN||key==VK_RWIN)return 0;
        const int mods=((GetKeyState(VK_CONTROL)&0x8000)?MOD_CONTROL:0)|((GetKeyState(VK_MENU)&0x8000)?MOD_ALT:0)|
            ((GetKeyState(VK_SHIFT)&0x8000)?MOD_SHIFT:0)|(((GetKeyState(VK_LWIN)|GetKeyState(VK_RWIN))&0x8000)?MOD_WIN:0);
        if(key==VK_TAB && mods==0) {SetFocus(GetNextDlgTabItem(GetParent(h),h,FALSE));return 0;}
        if(key==VK_ESCAPE && mods==0) {SendMessageW(GetParent(h),WM_CLOSE,0,0);return 0;}
        state->keys[role]=(mods==0&&(key==VK_BACK||key==VK_DELETE))?0:static_cast<int>(key);
        state->modifiers[role]=state->keys[role]?mods:0;
        SetWindowTextW(h,HotkeyLabel(state->keys[role],state->modifiers[role]).c_str());
        state->accessError.clear();InvalidateRect(GetParent(h),nullptr,FALSE);return 0;
    }
    if(message==WM_CHAR||message==WM_SYSCHAR)return 0;
    if(message==WM_MOUSEMOVE) {state->hotkeyHovered[role]=true;TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);RedrawWindow(h,nullptr,nullptr,RDW_FRAME|RDW_INVALIDATE);}
    if(message==WM_MOUSELEAVE) {state->hotkeyHovered[role]=false;RedrawWindow(h,nullptr,nullptr,RDW_FRAME|RDW_INVALIDATE);}
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS)RedrawWindow(h,nullptr,nullptr,RDW_FRAME|RDW_INVALIDATE);
    if(message==WM_NCPAINT) {lattice::ui::DrawDialogEditBorder(h,DialogPanelBrush(),state->hotkeyHovered[role],kDialogAccent,kDialogBorder,Scale(*state,10));return 0;}
    if(message==WM_NCDESTROY)RemoveWindowSubclass(h,HotkeyEditSubclass,id);
    return DefSubclassProc(h,message,key,param);
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
        if (id == kDisplayModeId) lattice::ui::DrawDialogCombo(hwnd, dc, state->textFont, state->dpi, GetFocus() == hwnd, {8});
        else DrawThemeCombo(hwnd, dc, *state);
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
            if (state->displayMode) {
                CreateLabel(state, L"分类与访问", 22, 366, 330, 26);
                CreateLabel(state, L"分类显示方式", 22, 406, 138, 28);
                state->displayCombo = CreateWindowW(L"COMBOBOX", L"",
                    WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,
                    Scale(*state,172),Scale(*state,400),Scale(*state,286),Scale(*state,120),hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDisplayModeId)),state->instance,nullptr);
                SetFont(state,state->displayCombo);
                SendMessageW(state->displayCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"多个格子"));
                SendMessageW(state->displayCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"标签容器"));
                SendMessageW(state->displayCombo,CB_SETCURSEL,*state->displayMode==0?1:0,0);
                SendMessageW(state->displayCombo,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),Scale(*state,36));
                SetWindowSubclass(state->displayCombo,ThemeComboSubclass,kDisplayModeId,reinterpret_cast<DWORD_PTR>(state));
                CreateLabel(state,L"同一分类与顺序；分别记住两种模式的布局。",22,450,436,34);
                CreateLabel(state,L"快速隐藏快捷键",22,498,138,28);
                CreateLabel(state,L"临时访问快捷键",22,550,138,28);
                state->keys[0]=state->settings->quickHideKey;state->modifiers[0]=state->settings->quickHideModifiers;
                state->keys[1]=state->settings->temporaryAccessKey;state->modifiers[1]=state->settings->temporaryAccessModifiers;
                for(int role=0;role<2;++role) {
                    HWND field=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",HotkeyLabel(state->keys[role],state->modifiers[role]).c_str(),
                        WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_READONLY|ES_AUTOHSCROLL,Scale(*state,172),Scale(*state,492+role*52),Scale(*state,286),Scale(*state,36),hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(role==0?kQuickHideId:kTemporaryAccessId)),state->instance,nullptr);
                    if(role==0)state->quickHide=field;else state->temporaryAccess=field;
                    SetFont(state,field);SetWindowTheme(field,L"",L"");
                    SetWindowSubclass(field,HotkeyEditSubclass,role==0?kQuickHideId:kTemporaryAccessId,reinterpret_cast<DWORD_PTR>(state));
                }
                CreateLabel(state,L"点击后按组合键；Backspace/Delete 清除以停用。\n临时访问：再次触发或 Esc 结束；菜单与拖放期间保持。",22,598,436,48);
                state->tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
                    0,0,0,0,hwnd,nullptr,state->instance,nullptr);
                SetWindowTheme(state->tooltip,L"",L"");
                SendMessageW(state->tooltip,TTM_SETTIPBKCOLOR,kDialogPanel,0);
                SendMessageW(state->tooltip,TTM_SETTIPTEXTCOLOR,kDialogText,0);
                SendMessageW(state->tooltip,TTM_SETMAXTIPWIDTH,0,Scale(*state,280));
                SendMessageW(state->tooltip,TTM_SETDELAYTIME,TTDT_INITIAL,500);
                TOOLINFOW ti{sizeof(ti)};ti.uFlags=TTF_IDISHWND|TTF_SUBCLASS;ti.hwnd=hwnd;
                ti.uId=reinterpret_cast<UINT_PTR>(state->displayCombo);
                ti.lpszText=const_cast<wchar_t*>(L"同一份分类和顺序；切换恢复本模式保存的布局");
                SendMessageW(state->tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&ti));
                ti.uId=reinterpret_cast<UINT_PTR>(state->quickHide);ti.lpszText=const_cast<wchar_t*>(L"点击后按组合键；冲突保留原绑定；Backspace/Delete 清除");
                SendMessageW(state->tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&ti));
                ti.uId=reinterpret_cast<UINT_PTR>(state->temporaryAccess);ti.lpszText=const_cast<wchar_t*>(L"再次触发或 Esc 结束，不因松键和普通失焦撤层");
                SendMessageW(state->tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&ti));
            }
            state->ok = CreateWindowW(
                L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
                Scale(*state, state->displayMode ? 372 : 208), Scale(*state, state->displayMode ? 710 : 370), Scale(*state, 86), Scale(*state, 32),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOkId)), state->instance, nullptr);
            state->cancel = CreateWindowW(
                L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                Scale(*state, state->displayMode ? 278 : 302), Scale(*state, state->displayMode ? 710 : 370), Scale(*state, 86), Scale(*state, 32),
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
            if (draw.CtlID == kThemeId || draw.CtlID == kDisplayModeId) {
                DrawThemeItem(draw, *state);
            } else {
                DrawButton(draw, *state);
            }
            return TRUE;
        }

        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure->CtlID == kThemeId || measure->CtlID == kDisplayModeId) {
                measure->itemHeight = Scale(*state, 26);
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kDialogText);
            const bool field=reinterpret_cast<HWND>(lParam)==state->quickHide || reinterpret_cast<HWND>(lParam)==state->temporaryAccess;
            SetBkColor(dc,field?kDialogPanel:kDialogBackground);
            SetBkMode(dc,field?OPAQUE:TRANSPARENT);
            return reinterpret_cast<LRESULT>(field?DialogPanelBrush():DialogBackgroundBrush());
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
            if (command == kDisplayModeId && HIWORD(wParam) == CBN_SELCHANGE) {
                InvalidateRect(state->displayCombo,nullptr,FALSE); return 0;
            }
            if (command == kThemeId && HIWORD(wParam) == CBN_SELCHANGE) {
                InvalidateRect(state->theme, nullptr, FALSE);
                return 0;
            }
            if (command == kOkId) {
                if(state->displayMode) {
                    AppSettings candidate=*state->settings;
                    candidate.quickHideKey=state->keys[0];candidate.quickHideModifiers=state->modifiers[0];
                    candidate.temporaryAccessKey=state->keys[1];candidate.temporaryAccessModifiers=state->modifiers[1];
                    if(state->validateAccess&&!state->validateAccess(candidate,state->accessError)) {InvalidateRect(hwnd,nullptr,FALSE);return 0;}
                    *state->settings=candidate;
                }
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
                if(state->displayMode) *state->displayMode=SendMessageW(state->displayCombo,CB_GETCURSEL,0,0)==1?0:1;
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
            if(state->displayMode&&!state->accessError.empty()) {
                SetTextColor(dc,RGB(255,158,158));SelectObject(dc,state->textFont);
                RECT error{Scale(*state,22),Scale(*state,653),Scale(*state,458),Scale(*state,701)};
                DrawTextW(dc,state->accessError.c_str(),-1,&error,DT_LEFT|DT_WORDBREAK);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if(state->tooltip) DestroyWindow(state->tooltip);
            DeleteObject(state->titleFont);
            DeleteObject(state->textFont);
            return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

bool SettingsDialog::Show(HINSTANCE instance, HWND owner, AppSettings& settings, int* displayMode, std::function<bool(const AppSettings&,std::wstring&)> validateAccess) {
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
    state.displayMode = displayMode;
    state.validateAccess=std::move(validateAccess);
    state.dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (state.dpi < 96) {
        state.dpi = 96;
    }
    state.width = Scale(state, displayMode ? 480 : 420);
    state.height = Scale(state, displayMode ? 762 : 420);
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
    MONITORINFO placementMonitor{};
    placementMonitor.cbSize = sizeof(placementMonitor);
    const HMONITOR monitor = MonitorFromRect(&ownerRect, MONITOR_DEFAULTTONEAREST);
    RECT workArea = ownerRect;
    if (GetMonitorInfoW(monitor, &placementMonitor)) workArea = placementMonitor.rcWork;
    const RECT placement = MessageDialog::CalculatePlacement(ownerRect, workArea, state.width, state.height);
    SetWindowPos(dialog, HWND_TOP, placement.left, placement.top, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
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
